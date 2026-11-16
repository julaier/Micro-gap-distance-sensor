/* V5: retain the V4 channel settings, quiet wait and test I2C primitives.
 * Only CONFIG bit9 changes: select the LDC's internal reference oscillator.
 * Verify manual-drive configuration while asleep, wait 1s, verify again,
 * and ONLY THEN clear SLEEP_MODE_EN. No automatic retry/reference fallback.
 * Capture diagnostics before UART output; no UART inside bus transactions.
 * Default: wait quietly for 5ms AFTER the wake transaction, including NACK.
 * A reference-path comparison, NOT proof that the write NACK is fixed.
 * No ERROR_CONFIG write: the original F103 driver did not enable DRDY_2INT.
 */
#include "wake_diag.h"
#include "wake_i2c.h"
#include "test_board.h"

#define LDC_ADDR 0x2Au
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
/* TI SNOSCY9A CONFIG table 27: bit12 RP_OVERRIDE_EN=1 disables initial
 * automatic sensor calibration; bit10 AUTO_AMP_DIS=1 disables amplitude
 * correction. Bit11 SENSOR_ACTIVATE_SEL=0 selects full-current activation,
 * as recommended by TI. This is NOT the separate HIGH_CURRENT_DRV bit6.
 * Bit9=0 selects the internal reference. MCU clock/GPIO/timing are unchanged.
 * Bit13 alone separates sleeping 3401 / awake 1401. Relative to V4
 * (3601/1601), only bit9 changes; no real-board result is presumed. */
#define CONFIG_RESET          0x2801u
#define CONFIG_SLEEP          0x2000u
#define CONFIG_MANUAL_AWAKE   0x1401u
#define CONFIG_MANUAL_SLEEP   (CONFIG_MANUAL_AWAKE | CONFIG_SLEEP)
#define STABLE_COUNT          11u

typedef struct { uint8_t reg; uint16_t value; } setting_t;
static const setting_t settings[] = {
    {0x14u, 0x1001u}, {0x15u, 0x1001u},
    {0x10u, 0x0180u}, {0x11u, 0x0180u},
    {0x08u, 0xFFFFu}, {0x09u, 0xFFFFu},
    {0x1Eu, 0x9000u}, {0x1Fu, 0x9000u}, {0x1Bu, 0x820Cu}
};
/* Stable configuration first; compare only the programmable IDRIVE field
 * of 1E/1F, not the read-only INIT_IDRIVE field. STATUS/raw are observation. */
static const setting_t observed[] = {
    {0x1Au, CONFIG_MANUAL_AWAKE}, {0x1Bu, 0x820Cu},
    {0x08u, 0xFFFFu}, {0x09u, 0xFFFFu},
    {0x10u, 0x0180u}, {0x11u, 0x0180u},
    {0x19u, 0x0000u}, {0x14u, 0x1001u}, {0x15u, 0x1001u},
    {0x1Eu, 0x9000u}, {0x1Fu, 0x9000u}, {0x18u, 0u},
    {0x00u, 0u}, {0x01u, 0u}, {0x02u, 0u}, {0x03u, 0u}
};
typedef struct {
    uint8_t reg, index;
    uint16_t value;
    int rc;
    test_i2c_diag_t bus;
} write_record_t;
typedef wake_snapshot_t snapshot_t;
typedef char observed_count_must_match[
    COUNT(observed) == WAKE_DIAG_REGISTER_COUNT ? 1 : -1];

volatile wake_debug_t g_wake_debug;
volatile uint32_t g_wake_quiet = 1u;

__attribute__((noinline)) void wake_diag_ready_checkpoint(void)
{
    g_wake_debug.checkpoint = 1u; /* Safe optional breakpoint: still asleep. */
}

__attribute__((noinline)) void wake_diag_done_checkpoint(void)
{
    g_wake_debug.checkpoint = 2u; /* Safe optional breakpoint: evidence saved. */
}

static write_record_t writes[COUNT(settings) + 3u];
static unsigned write_count;
static uint8_t attempted, configured;
static uint16_t target_config = CONFIG_MANUAL_SLEEP;

static void number(uint32_t value)
{
    char digits[10];
    unsigned n = 0u;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) test_uart_putc(digits[--n]);
}
static void signed_number(int value)
{
    if (value < 0) { test_uart_putc('-'); number(0u - (uint32_t)value); }
    else number((uint32_t)value);
}
static void hex(uint32_t value, unsigned digits)
{
    static const char alphabet[] = "0123456789ABCDEF";
    while (digits) { --digits; test_uart_putc(alphabet[(value >> (digits * 4u)) & 15u]); }
}
static void comma(void) { test_uart_putc(','); }
static void eol(void) { test_uart_puts("\r\n"); }

static int write_setting(uint8_t reg, uint16_t value)
{
    write_record_t *entry = &writes[write_count++];
    entry->reg = reg;
    entry->value = value;
    entry->rc = wake_i2c_write_reg(LDC_ADDR, reg, value, &entry->index);
    test_i2c_get_diag(&entry->bus);
    return entry->rc;
}

static void report_writes(void)
{
    unsigned i;
    for (i = 0u; i < write_count; ++i) {
        const write_record_t *entry = &writes[i];
        const test_i2c_diag_t *d = &entry->bus;
        test_uart_puts("$W,"); hex(entry->reg, 2u); comma(); hex(entry->value, 4u);
        comma(); signed_number(entry->rc); comma();
        test_uart_puts(test_i2c_stage_name(d->stage)); comma(); number(entry->index);
        comma(); hex(d->tx, 2u); comma(); number(d->ack_level);
        comma(); signed_number(d->stop_rc); comma(); number(d->clocks);
        comma(); number(d->hz); eol();
    }
}

static int ids(const char *tag)
{
    uint16_t mid = 0u, did = 0u;
    int mr = test_i2c_read_reg(LDC_ADDR, 0x7Eu, &mid);
    int dr = test_i2c_read_reg(LDC_ADDR, 0x7Fu, &did);
    test_uart_puts("$D,"); test_uart_puts(tag); comma(); signed_number(mr);
    comma(); hex(mid, 4u); comma(); signed_number(dr); comma(); hex(did, 4u); eol();
    if (mr != 0) return mr;
    if (dr != 0) return dr;
    return mid == 0x5449u && did == 0x3055u ? 0 : -11;
}

static void capture(volatile snapshot_t *snapshot)
{
    unsigned i;
    snapshot->captured = 0u;
    snapshot->hold = configured ? 0 : -4;
    snapshot->comm = 0;
    for (i = 0u; i < COUNT(observed); ++i) {
        uint16_t target = i == 0u ? target_config : observed[i].value;
        uint16_t value = 0u;
        int rc = test_i2c_read_reg(LDC_ADDR, observed[i].reg, &value);
        snapshot->values[i] = value;
        snapshot->results[i] = rc;
        if (rc != 0 && snapshot->comm == 0) snapshot->comm = rc;
        if (configured && i < STABLE_COUNT && snapshot->hold == 0) {
            uint16_t mask = i >= 9u ? 0xF800u : 0xFFFFu;
            if (rc != 0) snapshot->hold = rc;
            else if ((snapshot->values[i] & mask) != (target & mask)) snapshot->hold = -18;
        }
    }
    snapshot->captured = 1u;
}

static void report_snapshot(const char *tag, const volatile snapshot_t *snapshot)
{
    unsigned i;
    for (i = 0u; i < COUNT(observed); ++i) {
        test_uart_puts("$Q,"); test_uart_puts(tag); comma(); hex(observed[i].reg, 2u);
        comma(); signed_number(snapshot->results[i]);
        comma(); hex(snapshot->values[i], 4u); eol();
    }
    test_uart_puts("$K,"); test_uart_puts(tag); test_uart_puts(",COMM,");
    signed_number(snapshot->comm); eol();
    test_uart_puts("$K,"); test_uart_puts(tag); test_uart_puts(",HOLD,");
    signed_number(snapshot->hold); eol();
}

static int snapshot_result(const volatile snapshot_t *snapshot)
{
    /* Keep communication faults visible even if an earlier register already
     * mismatched. $K COMM and HOLD report both aspects independently. */
    return snapshot->comm != 0 ? snapshot->comm : snapshot->hold;
}

int wake_diag_start(void)
{
    uint16_t before = 0u, after = 0u;
    int rc, before_rc = -4, after_rc = -4, wake_rc = -4;
    int sleep_write_rc = -4;
    uint8_t sleep_captured = 0u, wake_attempted = 0u;
    const char *stage = "BUS_INIT";
    unsigned i;

    if (attempted) {
        test_uart_puts("$B,RESTART_BLOCKED,RESET_MCU_TO_RETEST\r\n");
        return TEST_I2C_ARGUMENT;
    }
    attempted = 1u;
    g_wake_debug.magic = 0x57414B35u; /* WAK5 */
    g_wake_debug.quiet_ms = g_wake_quiet ? 5u : 0u;
    g_wake_debug.result = -4;
    g_wake_debug.before_read_rc = g_wake_debug.sleep_write_rc = -4;
    g_wake_debug.wake_write_rc = g_wake_debug.wake_read_rc = -4;
    g_wake_debug.wake_payload_index = 255u;
    g_wake_debug.phase = WAKE_DEBUG_BUS_INIT;
    rc = test_i2c_init();
    test_uart_puts("$B,BUS_INIT,"); signed_number(rc); eol();
    if (rc != 0) goto done;

    stage = "RESET";
    g_wake_debug.phase = WAKE_DEBUG_RESET;
    rc = write_setting(0x1Cu, 0x8000u); /* The only LDC reset in this image. */
    if (rc != 0) goto done;
    test_delay_ms(5u);
    stage = "ID";
    g_wake_debug.phase = WAKE_DEBUG_ID;
    rc = ids("RESET_ID");
    if (rc != 0) goto done;

    /* Same channel write values/order as new/Device/ldc1614.c.
     * Do not print or insert delays in this group. */
    stage = "CONFIG_WRITE";
    g_wake_debug.phase = WAKE_DEBUG_CONFIG_WRITE;
    for (i = 0u; i < COUNT(settings); ++i) {
        rc = write_setting(settings[i].reg, settings[i].value);
        if (rc != 0) goto done;
    }
    stage = "CONFIG_READ";
    g_wake_debug.phase = WAKE_DEBUG_CONFIG_READ;
    before_rc = test_i2c_read_reg(LDC_ADDR, 0x1Au, &before);
    g_wake_debug.before_read_rc = before_rc;
    g_wake_debug.before_config = before;
    rc = before_rc;
    if (rc != 0) goto done;
    if (before != CONFIG_RESET) {
        stage = "RESET_CONFIG";
        rc = -18;
        goto done;
    }
    configured = 1u;
    target_config = CONFIG_MANUAL_SLEEP;
    stage = "SLEEP_WRITE";
    g_wake_debug.phase = WAKE_DEBUG_SLEEP_WRITE;
    sleep_write_rc = write_setting(0x1Au, target_config);
    g_wake_debug.sleep_write_rc = sleep_write_rc;
    g_wake_debug.phase = WAKE_DEBUG_SLEEP_NOW;
    capture(&g_wake_debug.sleep_now);
    test_delay_ms(1000u); /* No recovery/reinit/writes during this interval. */
    g_wake_debug.phase = WAKE_DEBUG_SLEEP_1S;
    capture(&g_wake_debug.sleep_1s);
    sleep_captured = 1u;
    rc = sleep_write_rc;
    if (rc != 0) goto done; /* Never forgive a sleep-write NACK. */
    stage = "SLEEP_NOW";
    rc = snapshot_result(&g_wake_debug.sleep_now);
    if (rc != 0) goto done;
    stage = "SLEEP_1S";
    rc = snapshot_result(&g_wake_debug.sleep_1s);
    if (rc != 0) goto done; /* Configuration must survive BEFORE waking. */

    g_wake_debug.phase = WAKE_DEBUG_READY;
    wake_diag_ready_checkpoint(); /* F5 from here; do NOT single-step I2C. */
    target_config = CONFIG_MANUAL_AWAKE;
    wake_attempted = 1u;
    g_wake_debug.wake_attempted = 1u;
    stage = "WAKE_WRITE";
    g_wake_debug.phase = WAKE_DEBUG_WAKE_WRITE;
    wake_rc = write_setting(0x1Au, target_config);
    g_wake_debug.wake_write_rc = wake_rc;
    g_wake_debug.wake_payload_index = writes[write_count - 1u].index;
    g_wake_debug.wake_bus = writes[write_count - 1u].bus;
    /* No UART, reads, reset, retry or clock switching in this interval.
     * A wait AFTER a NACK cannot retroactively repair the failed write. */
    g_wake_debug.phase = WAKE_DEBUG_QUIET;
    if (g_wake_debug.quiet_ms != 0u) test_delay_ms((uint16_t)g_wake_debug.quiet_ms);
    g_wake_debug.phase = WAKE_DEBUG_WAKE_READ;
    after_rc = test_i2c_read_reg(LDC_ADDR, 0x1Au, &after);
    g_wake_debug.wake_read_rc = after_rc;
    g_wake_debug.after_config = after;
    rc = wake_rc;
    if (rc != 0) goto done;
    stage = "WAKE_READ";
    rc = after_rc;
    if (rc != 0) goto done;
    stage = "WAKE_VALUE";
    rc = after == target_config ? 0 : -18;

done:
    /* Capture both sequential register sets before printing the records.
     * PLUS20MS means a 20ms wait after FIRST_READS, not 20ms after wake. */
    g_wake_debug.phase = WAKE_DEBUG_FIRST_READS;
    capture(&g_wake_debug.first_reads);
    if (wake_attempted) {
        test_delay_ms(20u);
        g_wake_debug.phase = WAKE_DEBUG_PLUS20MS;
        capture(&g_wake_debug.plus20ms);
    }
    report_writes();
    test_uart_puts("$B,WAKE_QUIET_MS,"); number(g_wake_debug.quiet_ms); eol();
    test_uart_puts("$B,CONFIG_BEFORE,"); signed_number(before_rc);
    comma(); hex(before, 4u); eol();
    test_uart_puts("$S,1A,"); hex(CONFIG_MANUAL_SLEEP, 4u);
    comma(); signed_number(sleep_write_rc);
    comma(); signed_number(sleep_captured ? g_wake_debug.sleep_now.results[0] : -4);
    comma(); hex(sleep_captured ? g_wake_debug.sleep_now.values[0] : 0u, 4u); eol();
    if (sleep_captured) {
        report_snapshot("SLEEP_NOW", &g_wake_debug.sleep_now);
        report_snapshot("SLEEP_1S", &g_wake_debug.sleep_1s);
    }
    test_uart_puts("$C,1A,"); hex(CONFIG_MANUAL_AWAKE, 4u); comma(); signed_number(wake_rc);
    comma(); signed_number(after_rc); comma(); hex(after, 4u); eol();
    if (wake_attempted) {
        report_snapshot("FIRST_READS", &g_wake_debug.first_reads);
        report_snapshot("PLUS20MS", &g_wake_debug.plus20ms);
    } else {
        report_snapshot("FINAL", &g_wake_debug.first_reads);
        test_uart_puts("$K,WAKE,SKIPPED,-4\r\n");
    }
    if (rc == 0 && wake_attempted &&
        (snapshot_result(&g_wake_debug.first_reads) != 0 ||
         snapshot_result(&g_wake_debug.plus20ms) != 0)) {
        stage = "HOLD";
        rc = snapshot_result(&g_wake_debug.first_reads) != 0 ?
            snapshot_result(&g_wake_debug.first_reads) : snapshot_result(&g_wake_debug.plus20ms);
    }
    test_uart_puts("$B,RESULT,");
    test_uart_puts(rc == 0 ? "CONFIG_PASS_NOT_MEASUREMENT" : stage);
    comma(); signed_number(rc); eol();
    test_uart_puts("$B,READ_ONLY_MONITOR,NO_RESET_NO_RETRY\r\n");
    g_wake_debug.result = rc;
    g_wake_debug.result_stage = rc == 0 ? "CONFIG_PASS_NOT_MEASUREMENT" : stage;
    g_wake_debug.done = 1u;
    g_wake_debug.phase = WAKE_DEBUG_DONE;
    wake_diag_done_checkpoint();
    return rc;
}

void wake_diag_monitor(uint32_t round)
{
    snapshot_t snapshot;
    test_uart_puts("$B,ROUND,"); number(round);
    comma(); number(test_bus_lines()); comma(); number((uint32_t)test_intb_read()); eol();
    (void)ids("MONITOR_ID");
    capture(&snapshot);
    report_snapshot("MONITOR", &snapshot);
    test_uart_puts("$B,ROUND_END\r\n");
}
