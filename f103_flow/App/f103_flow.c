/* Calls the unchanged original F103 LDC configuration implementation.
 * No intermediate UART/readback/sleep diagnostic is inserted into that call.
 * This is an initialization/raw-data validation project, not distance firmware.
 */
#include "f103_flow.h"
#include "flow_bus.h"
#include "ldc1614.h"
#include "test_board.h"

volatile flow_debug_t g_flow_debug;
static unsigned attempted;
static const uint8_t observed[FLOW_OBSERVED] = {
    0x1A,0x1B,0x08,0x09,0x10,0x11,0x14,0x15,0x19,0x1E,0x1F,0x18,0,1,2,3
};
static const uint16_t expected[9] = {
    0x0A01,0x820C,0xFFFF,0xFFFF,0x0180,0x0180,0x1001,0x1001,0
};

static void number(uint32_t v)
{
    char b[10]; unsigned n = 0u;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) test_uart_putc(b[--n]);
}
static void signed_number(int v)
{
    if (v < 0) { test_uart_putc('-'); number(0u - (uint32_t)v); }
    else number((uint32_t)v);
}
static void hex(uint32_t v, unsigned n)
{
    const char *d = "0123456789ABCDEF";
    while (n) { --n; test_uart_putc(d[(v >> (n * 4u)) & 15u]); }
}
static void comma(void) { test_uart_putc(','); }
static void eol(void) { test_uart_puts("\r\n"); }

__attribute__((noinline)) void f103_flow_done_checkpoint(void)
{
    g_flow_debug.checkpoint = 1u; /* Safe optional breakpoint AFTER all startup I2C. */
}

static void snapshot(volatile flow_snapshot_t *s)
{
    unsigned i;
    s->comm = 0;
    s->hold = g_flow_debug.init_rc == 0 ? 0 : -4;
    for (i = 0u; i < FLOW_OBSERVED; ++i) {
        uint16_t v = 0u;
        int rc = ldc1614_read_reg(observed[i], &v);
        s->value[i] = v; s->rc[i] = rc;
        if (rc != 0 && s->comm == 0) s->comm = rc;
        /* Auto-amplitude can change IDRIVE: don't compare 1E/1F to 9000 awake. */
        if (i < 9u && s->hold == 0) {
            if (rc != 0) s->hold = rc;
            else if (v != expected[i]) s->hold = -18;
        }
    }
}

static void report_snapshot(const char *tag, const volatile flow_snapshot_t *s)
{
    unsigned i;
    for (i = 0u; i < FLOW_OBSERVED; ++i) {
        test_uart_puts("$Q,"); test_uart_puts(tag); comma(); hex(observed[i], 2u);
        comma(); signed_number(s->rc[i]); comma(); hex(s->value[i], 4u); eol();
    }
    test_uart_puts("$K,"); test_uart_puts(tag); test_uart_puts(",COMM,");
    signed_number(s->comm); eol();
    test_uart_puts("$K,"); test_uart_puts(tag); test_uart_puts(",HOLD,");
    signed_number(s->hold); eol();
}

static void report_record(const char *tag, const volatile flow_record_t *r)
{
    test_uart_puts(tag); test_uart_putc((char)r->operation);
    comma(); hex(r->reg, 2u); comma(); signed_number(r->rc);
    comma(); hex(r->value, 4u); comma();
    test_uart_puts(test_i2c_stage_name(r->bus.stage));
    comma(); number(r->payload_index); comma(); hex(r->bus.tx, 2u);
    comma(); signed_number(r->bus.stop_rc); comma(); number(r->bus.hz); eol();
}
static void report_startup(void)
{
    unsigned i;
    test_uart_puts("$B,BUS_INIT,"); signed_number(g_flow_bus.init_rc); eol();
    for (i = 0u; i < g_flow_bus.count; ++i)
        report_record("$F,STEP,", &g_flow_bus.records[i]);
    test_uart_puts("$B,TRACE_OVERFLOW,"); number(g_flow_bus.overflow); eol();
    if (g_flow_bus.first_failed) {
        report_record("$F,FIRST,", &g_flow_bus.first_failure);
        for (i = 0u; i < g_flow_bus.first_ack.count; ++i) {
            const volatile ack_sample_t *a = &g_flow_bus.first_ack.bytes[i];
            test_uart_puts("$A,FIRST,"); number(a->slot);
            comma(); hex(a->tx, 2u); comma(); number(a->samples);
            comma(); number(a->sda_mask); comma(); number(a->scl_mask);
            comma(); signed_number(a->rc);
#ifdef MARGIN_I2C
            comma(); number(a->dt_us[0]); comma(); number(a->dt_us[1]);
            comma(); number(a->dt_us[2]); comma(); number(a->high_us);
#endif
            eol();
        }
    } else test_uart_puts("$F,FIRST,NONE\r\n");
    test_uart_puts("$D,INIT_IDS,"); hex(g_flow_debug.manufacturer_id, 4u);
    comma(); hex(g_flow_debug.device_id, 4u); eol();
    test_uart_puts("$B,ORIGINAL_INIT_RC,"); signed_number(g_flow_debug.init_rc); eol();
}

int f103_flow_start(void)
{
    uint16_t did = 0u, mid = 0u;
    int rc;
    if (attempted) return TEST_I2C_ARGUMENT;
    attempted = 1u;
    g_flow_debug.magic = 0x46313033u; /* F103 */
    ldc1614_init_driver();
    rc = g_flow_bus.init_rc;
    if (rc == 0) rc = ldc1614_init(&did, &mid);
    /* Freeze BEFORE any later diagnostic register-pointer writes. */
    flow_bus_freeze();
    g_flow_debug.init_rc = rc;
    g_flow_debug.manufacturer_id = mid;
    g_flow_debug.device_id = did;
    snapshot(&g_flow_debug.startup);
    g_flow_debug.result = rc != 0 ? rc :
        (g_flow_debug.startup.comm != 0 ? g_flow_debug.startup.comm : g_flow_debug.startup.hold);
    report_startup();
    report_snapshot("STARTUP", &g_flow_debug.startup);
    test_uart_puts("$B,RESULT,");
    test_uart_puts(g_flow_debug.result == 0 ? "CONFIG_PASS_NOT_MEASUREMENT" :
        (rc != 0 ? "ORIGINAL_INIT" : "POST_INIT_READBACK"));
    comma(); signed_number(g_flow_debug.result); eol();
    test_uart_puts("$B,NO_RESET_NO_RETRY,RAW_POLL_ONLY_AFTER_CONFIG_PASS\r\n");
    g_flow_debug.done = 1u;
    f103_flow_done_checkpoint();
    return g_flow_debug.result;
}

void f103_flow_poll(void)
{
    uint16_t status = 0u, errors = 0u;
    uint32_t a = 0u, b = 0u;
    int rc;
    if (!g_flow_debug.done || g_flow_debug.result != 0) return;
    rc = ldc1614_read_status(&status);
    if (rc == 0) {
        if ((status & 0x000Cu) != 0x000Cu) rc = 1; /* Both unread flags required. */
        else {
            rc = ldc1614_read_raw(&a, &b, &errors);
            if (rc == 0) {
                ++g_flow_debug.new_pairs;
                if (a != 0u && b != 0u && errors == 0u) ++g_flow_debug.valid_pairs;
                else rc = 2; /* Raw pair read, but NOT a valid measurement. */
            }
        }
    }
    g_flow_debug.status = status; g_flow_debug.data_errors = errors;
    g_flow_debug.ch0 = a; g_flow_debug.ch1 = b; g_flow_debug.poll_rc = rc;
}

void f103_flow_monitor(uint32_t round)
{
    flow_snapshot_t s;
    test_uart_puts("$B,ROUND,"); number(round); eol();
    /* Do not read DATA MSB/LSB here during successful polling: MSB reads can
     * clear unread flags. Startup failure monitoring may inspect all DATA. */
    if (g_flow_debug.result == 0) {
        test_uart_puts("$POLL,"); signed_number(g_flow_debug.poll_rc);
        comma(); hex(g_flow_debug.status, 4u);
        comma(); number(g_flow_debug.ch0); comma(); number(g_flow_debug.ch1);
        comma(); hex(g_flow_debug.data_errors, 4u);
        comma(); number(g_flow_debug.new_pairs); comma(); number(g_flow_debug.valid_pairs); eol();
    } else {
        snapshot(&s);
        report_snapshot("MONITOR", &s);
    }
    test_uart_puts("$B,ROUND_END\r\n");
}
