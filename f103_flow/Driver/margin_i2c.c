/* Independent, timer-scheduled OD driver. Do NOT compile wake_i2c.c as well.
 * PB7=SCL, PB6=SDA; no GPIO direction changes during a transaction.
 * The original F103 register order/configuration is outside this file.
 */
#include "margin_i2c.h"
#include "margin_port.h"
#include "test_board.h"

volatile margin_timing_t g_margin_last, g_margin_first_error;
static margin_timing_t timing;
static test_i2c_diag_t diag;
static ack_trace_t trace;
static uint16_t low_at, high_at, rise_at;
static uint8_t high_active, have_rise, started;
static uint32_t irq_prior;

static void wait_from(uint16_t since, uint16_t us)
{
    /* +1 tick: a read just before the next tick must not shorten the wait. */
    while ((uint16_t)(margin_now() - since) < (uint16_t)(us + 1u)) { }
}
static void wait_us(uint16_t us) { wait_from(margin_now(), us); }
static void range(uint16_t v, uint16_t *small, uint16_t *large)
{
    if (v < *small) *small = v;
    if (v > *large) *large = v;
}
static void reset_diag(void)
{
    diag = (test_i2c_diag_t){0};
    diag.ack_level = 255u;
    diag.lines = margin_pins();
    trace = (ack_trace_t){0};
    timing = (margin_timing_t){0};
    timing.low_min = timing.high_min = timing.period_min = ACK_TIME_UNKNOWN;
    high_active = have_rise = started = 0u;
}
static int clock_low(void)
{
    uint16_t began;
    margin_scl(0);
    began = margin_now();
    while (margin_pins() & 1u) {
        if ((uint16_t)(margin_now() - began) >= MARGIN_TIMEOUT_US)
            return TEST_I2C_BUS;
    }
    low_at = margin_now();
    /* Bookkeeping happens AFTER falling, never before an ACK sample. */
    if (high_active) {
        range((uint16_t)(low_at - high_at), &timing.high_min, &timing.high_max);
        high_active = 0u;
    }
    wait_from(low_at, MARGIN_HOLD_US);
    return 0;
}
static int clock_high(void)
{
    uint16_t began, low_time;
    wait_from(low_at, MARGIN_LOW_US);
    low_time = (uint16_t)(margin_now() - low_at);
    range(low_time, &timing.low_min, &timing.low_max);
    began = margin_now();
    margin_scl(1);
    /* LDC does not stretch clocks. Still verify the physical line, so a
     * slow rise/stuck bus cannot be mistaken for the start of a high phase. */
    while (!(margin_pins() & 1u)) {
        if ((uint16_t)(margin_now() - began) >= MARGIN_TIMEOUT_US)
            return TEST_I2C_BUS;
    }
    high_at = margin_now();
    high_active = 1u;
    return 0;
}
static int idle(void)
{
    uint16_t began = margin_now(), stable = began;
    margin_sda(1); margin_scl(1);
    for (;;) {
        uint8_t pins = margin_pins();
        uint16_t now = margin_now();
        if (pins != 3u) stable = now;
        else if ((uint16_t)(now - stable) >= MARGIN_FREE_US + 1u) return 0;
        if ((uint16_t)(now - began) >= MARGIN_TIMEOUT_US) return TEST_I2C_BUS;
    }
}
static int begin(int repeated)
{
    if (!repeated) {
        if (idle() != 0) return TEST_I2C_BUS;
    } else {
        /* SDA changes ONLY with SCL low; no extra SDA pulse after START. */
        margin_sda(1);
        wait_us(MARGIN_SETUP_US);
        if (clock_high() != 0) return TEST_I2C_BUS;
        if (!(margin_pins() & 2u)) return TEST_I2C_BUS;
    }
    wait_us(MARGIN_START_US); /* tSU;STA (or stable initial idle). */
    if (margin_pins() != 3u) return TEST_I2C_BUS;
    margin_sda(0);
    started = 1u;
    wait_us(MARGIN_START_US); /* tHD;STA before first falling SCL. */
    high_active = 0u; /* START/reSTART high is not a data clock. */
    have_rise = 0u;
    return clock_low();
}
static int end(void)
{
    if (!started) {
        margin_scl(1); margin_sda(1); /* No fake clocks on failed idle. */
        return margin_pins() == 3u ? 0 : TEST_I2C_BUS;
    }
    /* Caller has completed the ninth falling edge for ACK AND NACK. */
    if (clock_low() != 0) return TEST_I2C_BUS;
    margin_sda(0);
    wait_us(MARGIN_SETUP_US);
    if (clock_high() != 0) {
        margin_sda(1); margin_scl(1);
        return TEST_I2C_BUS; /* No SDA rising presented as STOP on low SCL. */
    }
    wait_from(high_at, MARGIN_STOP_US);
    margin_sda(1);
    high_active = started = 0u;
    return idle(); /* Verify both lines high for a complete tBUF. */
}
static int transmit(uint8_t value)
{
    ack_sample_t s = {0};
    unsigned bit;
    uint8_t p0, p1, p2;
    uint16_t first = 0u, ninth = 0u, dt0, dt1, dt2, width;
    int rc;
    s.slot = (uint8_t)trace.count; s.tx = diag.tx = value;
    s.high_us = s.dt_us[0] = s.dt_us[1] = s.dt_us[2] = ACK_TIME_UNKNOWN;
    diag.clocks = 0u; diag.ack_level = 255u; diag.hz = 0u;
    for (bit = 0u; bit < 8u; ++bit) {
        margin_sda((value & 0x80u) != 0u);
        wait_us(MARGIN_SETUP_US);
        rc = clock_high();
        if (rc != 0) goto save;
        if (bit == 0u) first = high_at;
        wait_from(high_at, MARGIN_HIGH_US);
        rc = clock_low();
        if (rc != 0) goto save;
        /* All counters/ranges are updated on LOW, not in a sampling window. */
        if (have_rise) range((uint16_t)(high_at - rise_at), &timing.period_min, &timing.period_max);
        rise_at = high_at; have_rise = 1u;
        ++diag.clocks; ++timing.clocks;
        value = (uint8_t)(value << 1);
    }
    margin_sda(1); /* Master RELEASES SDA before slave ACK. */
    wait_us(MARGIN_SETUP_US);
    rc = clock_high();
    if (rc != 0) goto save;
    ninth = high_at;
    /* Absolute deadlines within ONE ninth high. No UART, division, struct
     * copy, external function calls, or GPIO direction changes in window. */
    wait_from(ninth, 5u); p0 = margin_pins(); dt0 = (uint16_t)(margin_now() - ninth);
    wait_from(ninth, 9u); p1 = margin_pins(); dt1 = (uint16_t)(margin_now() - ninth);
    wait_from(ninth, 13u); p2 = margin_pins(); dt2 = (uint16_t)(margin_now() - ninth);
    wait_from(ninth, MARGIN_HIGH_US);
    rc = clock_low();
    width = (uint16_t)(low_at - ninth);
    if (rc != 0) goto save;
    ++diag.clocks; ++timing.clocks;
    range((uint16_t)(ninth - rise_at), &timing.period_min, &timing.period_max);
    rise_at = ninth;
    s.samples = 3u;
    s.sda_mask = (uint8_t)(((p0 >> 1) & 1u) | (p1 & 2u) | ((p2 & 2u) << 1));
    s.scl_mask = (uint8_t)((p0 & 1u) | ((p1 & 1u) << 1) | ((p2 & 1u) << 2));
    s.dt_us[0] = dt0; s.dt_us[1] = dt1; s.dt_us[2] = dt2; s.high_us = width;
    diag.hz = (uint16_t)(ninth - first) ? 8000000u / (uint16_t)(ninth - first) : 0u;
    if (s.scl_mask != 7u) rc = TEST_I2C_BUS;
    else if (s.sda_mask == 0u) { rc = 0; diag.ack_level = 0u; }
    else if (s.sda_mask == 7u) { rc = TEST_I2C_NACK; diag.ack_level = 1u; }
    else rc = ACK_I2C_UNSTABLE; /* An unstable ACK is never voted into OK. */
save:
    s.rc = rc; s.clocks = diag.clocks;
    if (trace.count < 4u) trace.bytes[trace.count++] = s;
    return rc;
}
static int receive(uint8_t *out, int last)
{
    unsigned bit;
    uint8_t value = 0u, pins;
    int rc;
    margin_sda(1);
    for (bit = 0u; bit < 8u; ++bit) {
        wait_us(MARGIN_SETUP_US);
        if (clock_high() != 0) return TEST_I2C_BUS;
        wait_from(high_at, 9u);
        pins = margin_pins();
        wait_from(high_at, MARGIN_HIGH_US);
        rc = clock_low();
        if (rc != 0 || !(pins & 1u)) return TEST_I2C_BUS;
        value = (uint8_t)((value << 1) | ((pins >> 1) & 1u));
        ++timing.clocks;
        if (have_rise) range((uint16_t)(high_at - rise_at), &timing.period_min, &timing.period_max);
        rise_at = high_at; have_rise = 1u;
    }
    margin_sda(last != 0); /* Master ACKs MSB, NACKs LSB. */
    wait_us(MARGIN_SETUP_US);
    if (clock_high() != 0) return TEST_I2C_BUS;
    wait_from(high_at, MARGIN_HIGH_US);
    rc = clock_low();
    if (rc != 0) return rc;
    ++timing.clocks;
    range((uint16_t)(high_at - rise_at), &timing.period_min, &timing.period_max);
    rise_at = high_at;
    margin_sda(1);
    *out = value;
    return 0;
}
static int finish(int rc)
{
    diag.lines = margin_pins();
    diag.stop_rc = end();
    if (rc == 0 && diag.stop_rc != 0) { rc = diag.stop_rc; diag.stage = TEST_STAGE_STOP; }
    else if (rc == 0) { diag.stage = TEST_STAGE_DONE; diag.lines = margin_pins(); }
    diag.rc = timing.rc = rc; timing.stop_rc = diag.stop_rc; timing.captured = 1u;
    g_margin_last = timing;
    if (rc != 0 && !g_margin_first_error.captured) g_margin_first_error = timing;
    margin_unlock(irq_prior);
    return rc;
}
int test_i2c_init(void)
{
    int rc;
    irq_prior = margin_lock(); reset_diag();
    /* Board init already selected OD. No recovery pulses or MODER toggles. */
    rc = idle();
    diag.rc = timing.rc = rc; diag.lines = margin_pins(); timing.captured = 1u;
    g_margin_last = timing;
    if (rc != 0 && !g_margin_first_error.captured) g_margin_first_error = timing;
    margin_unlock(irq_prior);
    return rc;
}
int test_i2c_probe(uint8_t addr7)
{
    int rc;
    if (addr7 > 127u) return TEST_I2C_ARGUMENT;
    irq_prior = margin_lock(); reset_diag();
    diag.stage = TEST_STAGE_START; rc = begin(0);
    if (rc == 0) { diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    return finish(rc);
}
int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value, uint8_t *index)
{
    const uint8_t bytes[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    unsigned i;
    int rc;
    if (index) *index = 255u;
    if (addr7 > 127u) return TEST_I2C_ARGUMENT;
    irq_prior = margin_lock(); reset_diag();
    diag.stage = TEST_STAGE_START; rc = begin(0);
    if (rc == 0) { diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    for (i = 0u; rc == 0 && i < 3u; ++i) {
        if (index) *index = (uint8_t)i;
        diag.stage = TEST_STAGE_REG; rc = transmit(bytes[i]);
    }
    return finish(rc);
}
int test_i2c_read_reg(uint8_t addr7, uint8_t reg, uint16_t *value)
{
    uint8_t msb = 0u, lsb = 0u;
    int rc;
    if (value) *value = 0u;
    if (addr7 > 127u || !value) return TEST_I2C_ARGUMENT;
    irq_prior = margin_lock(); reset_diag();
    diag.stage = TEST_STAGE_START; rc = begin(0);
    if (rc == 0) { diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    if (rc == 0) { diag.stage = TEST_STAGE_REG; rc = transmit(reg); }
    if (rc == 0) { diag.stage = TEST_STAGE_RESTART; rc = begin(1); }
    if (rc == 0) { diag.stage = TEST_STAGE_ADDR_R; rc = transmit((uint8_t)((addr7 << 1) | 1u)); }
    if (rc == 0) { diag.stage = TEST_STAGE_READ_MSB; rc = receive(&msb, 0); }
    if (rc == 0) { diag.stage = TEST_STAGE_READ_LSB; rc = receive(&lsb, 1); }
    rc = finish(rc);
    if (rc == 0) *value = (uint16_t)(((uint16_t)msb << 8) | lsb);
    return rc;
}
void test_i2c_get_diag(test_i2c_diag_t *out) { if (out) *out = diag; }
void ack_i2c_get_trace(volatile ack_trace_t *out) { if (out) *out = trace; }
const char *test_i2c_stage_name(test_i2c_stage_t stage)
{
    static const char *const names[] = {"IDLE","START","ADDR_W","REG","RESTART",
        "ADDR_R","READ_MSB","READ_LSB","STOP","DONE"};
    return (unsigned)stage < sizeof(names)/sizeof(names[0]) ? names[stage] : "IDLE";
}
static void print_number(uint32_t v)
{
    char buf[10]; unsigned n = 0u;
    do { buf[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) test_uart_putc(buf[--n]);
}
static void print_timing(const char *tag, const volatile margin_timing_t *m)
{
    const uint16_t values[] = {m->low_min,m->low_max,m->high_min,m->high_max,
        m->period_min,m->period_max};
    unsigned i;
    test_uart_puts("$M,"); test_uart_puts(tag);
    test_uart_putc(','); print_number(m->captured);
    for (i = 0u; i < 6u; ++i) { test_uart_putc(','); print_number(values[i]); }
    test_uart_putc(','); print_number(m->clocks);
    test_uart_putc(',');
    if (m->rc < 0) { test_uart_putc('-'); print_number(0u - (uint32_t)m->rc); }
    else print_number((uint32_t)m->rc);
    test_uart_puts("\r\n");
}
void margin_i2c_report(void)
{
    test_uart_puts("$B,TIMING_EVIDENCE,TIMER_ESTIMATES_NOT_SCOPE,65535_IS_UNKNOWN\r\n");
    print_timing("FIRST_ERROR", &g_margin_first_error);
    print_timing("LAST", &g_margin_last);
}
