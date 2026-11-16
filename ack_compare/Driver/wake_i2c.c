/* A deliberately includes the unchanged V5 adapter/primitives. B does not. */
#include "wake_i2c.h"
#include "test_board.h"

static ack_trace_t trace;

#if ACK_COMPARE_PROFILE == 0
#define wake_i2c_write_reg ack_legacy_write_reg
#include "../../wake_test/Driver/wake_i2c.c"
#undef wake_i2c_write_reg

int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value,
                       uint8_t *payload_index)
{
    uint8_t index = 255u;
    unsigned i;
    int rc = ack_legacy_write_reg(addr7, reg, value, &index);
    /* Only after STOP: leave every legacy in-transaction instruction intact. */
    trace.count = 0u;
    if (s_diag.ack_level <= 1u && s_diag.clocks == 9u) {
        ack_sample_t *s = &trace.bytes[0];
        s->slot = index == 255u ? 0u : (uint8_t)(index + 1u);
        s->tx = s_diag.tx;
        s->samples = 1u;
        s->sda_mask = s_diag.ack_level;
        s->scl_mask = 255u; /* Legacy did not sample SCL with SDA. */
        s->clocks = s_diag.clocks;
        for (i = 0u; i < 3u; ++i) s->dt_us[i] = ACK_TIME_UNKNOWN;
        s->high_us = ACK_TIME_UNKNOWN;
        s->rc = s_diag.ack_level ? TEST_I2C_NACK : 0;
        trace.count = 1u;
    }
    if (payload_index != 0) *payload_index = index;
    return rc;
}

#else
/* Independent B: SDA always stays OD output. ODR=1 releases it; receive
 * and ACK sample GPIO IDR. No MODER switches, IRQs, UART or calibration.
 * 5us requested setup/high plus software overhead, not a measured 100kHz.
 * 1us requested data hold after EACH falling SCL edge. ACK is sampled
 * three times during ONE ninth high, without adding extra SCL pulses.
 */
#define HALF_US 5u
#define TIMEOUT_US 2000u
static test_i2c_diag_t s_diag;

static void reset_diag(void)
{
    s_diag.rc = s_diag.stop_rc = 0;
    s_diag.stage = TEST_STAGE_IDLE;
    s_diag.tx = s_diag.clocks = 0u;
    s_diag.ack_level = 255u;
    s_diag.lines = ack_port_sample();
    s_diag.hz = 0u;
    trace.count = 0u;
}

static void low(void)
{
    test_scl_write(0);
    test_delay_us(1u); /* Hold previous data across SCL falling. */
}

static int high(void)
{
    uint16_t began;
    test_scl_write(1);
    began = test_time_us();
    while (!test_scl_read()) {
        if ((uint16_t)(test_time_us() - began) >= TIMEOUT_US)
            return TEST_I2C_BUS;
    }
    return 0;
}

static int begin(void)
{
    test_sda_write(1);
    test_delay_us(HALF_US);
    if (high() != 0) return TEST_I2C_BUS;
    test_delay_us(HALF_US);
    if (!test_sda_read()) return TEST_I2C_BUS;
    test_sda_write(0);
    test_delay_us(HALF_US);
    low();
    return 0;
}

static int end(void)
{
    int rc;
    low();
    test_sda_write(0);
    test_delay_us(HALF_US);
    rc = high();
    test_delay_us(HALF_US);
    test_sda_write(1);
    test_delay_us(HALF_US);
    if (rc != 0 || ack_port_sample() != 3u) return TEST_I2C_BUS;
    return 0;
}

static int transmit(uint8_t tx)
{
    ack_sample_t sample = {0};
    uint16_t first = 0u, ninth, elapsed;
    unsigned bit, k;
    int rc;
    sample.slot = (uint8_t)trace.count;
    sample.tx = s_diag.tx = tx;
    sample.high_us = ACK_TIME_UNKNOWN;
    for (k = 0u; k < 3u; ++k) sample.dt_us[k] = ACK_TIME_UNKNOWN;
    s_diag.clocks = 0u;
    s_diag.ack_level = 255u;
    s_diag.hz = 0u;
    for (bit = 0u; bit < 8u; ++bit) {
        test_sda_write((tx & 0x80u) != 0u);
        test_delay_us(HALF_US);
        if (high() != 0) { rc = TEST_I2C_BUS; goto save; }
        if (bit == 0u) first = test_time_us();
        ++s_diag.clocks;
        test_delay_us(HALF_US);
        low();
        tx = (uint8_t)(tx << 1);
    }
    test_sda_write(1); /* OD release BEFORE ninth rising edge. */
    test_delay_us(HALF_US);
#if ACK_COMPARE_PROFILE == 2
    /* Capture immediately around SCL release, BEFORE timer/function overhead
     * in high(). Then verify SCL really rose. Early samples NEVER override
     * the strict three-point result below. A/B preprocessed paths unchanged. */
    ack_port_capture_edge(&sample.edge);
    {
        uint16_t began = test_time_us();
        while (!test_scl_read()) {
            if ((uint16_t)(test_time_us() - began) >= TIMEOUT_US) {
                rc = TEST_I2C_BUS;
                goto save;
            }
        }
    }
#else
    if (high() != 0) { rc = TEST_I2C_BUS; goto save; }
#endif
    ninth = test_time_us();
    ++s_diag.clocks;
    for (k = 0u; k < 3u; ++k) {
        uint8_t pins;
        test_delay_us(k == 0u ? 1u : 2u);
        pins = ack_port_sample(); /* Atomic SCL+SDA observation. */
        sample.dt_us[k] = (uint16_t)(test_time_us() - ninth);
        if (pins & 1u) sample.scl_mask |= (uint8_t)(1u << k);
        if (pins & 2u) sample.sda_mask |= (uint8_t)(1u << k);
        ++sample.samples;
    }
    /* Lower clock BEFORE trace copying, decisions or 32-bit division. */
    test_scl_write(0);
    sample.high_us = (uint16_t)(test_time_us() - ninth);
    test_delay_us(1u);
    if (sample.scl_mask != 7u) rc = TEST_I2C_BUS;
    else if (sample.sda_mask == 0u) { rc = 0; s_diag.ack_level = 0u; }
    else if (sample.sda_mask == 7u) { rc = TEST_I2C_NACK; s_diag.ack_level = 1u; }
    else rc = ACK_I2C_UNSTABLE; /* Never vote an unstable ACK into success. */
    elapsed = (uint16_t)(ninth - first);
    if (elapsed) s_diag.hz = 8000000u / elapsed;
save:
    if (s_diag.clocks < 9u) low();
    sample.rc = rc;
    sample.clocks = s_diag.clocks;
    if (trace.count < 4u) trace.bytes[trace.count++] = sample;
    return rc;
}

static int receive(uint8_t *out, int last)
{
    unsigned bit;
    uint8_t value = 0u;
    test_sda_write(1);
    for (bit = 0u; bit < 8u; ++bit) {
        test_delay_us(HALF_US);
        if (high() != 0) { low(); return TEST_I2C_BUS; }
        test_delay_us(HALF_US);
        value = (uint8_t)((value << 1) | (test_sda_read() ? 1u : 0u));
        low();
    }
    test_sda_write(last != 0); /* Master ACK first byte, NACK final byte. */
    test_delay_us(HALF_US);
    if (high() != 0) { low(); test_sda_write(1); return TEST_I2C_BUS; }
    test_delay_us(HALF_US);
    low();
    test_sda_write(1);
    *out = value;
    return 0;
}

static int finish(int rc)
{
    s_diag.lines = ack_port_sample();
    s_diag.stop_rc = end();
    if (rc == 0 && s_diag.stop_rc != 0) {
        rc = TEST_I2C_BUS;
        s_diag.stage = TEST_STAGE_STOP;
        s_diag.lines = ack_port_sample();
    } else if (rc == 0) {
        s_diag.stage = TEST_STAGE_DONE;
        s_diag.lines = ack_port_sample();
    }
    s_diag.rc = rc;
    return rc;
}

int test_i2c_init(void)
{
    unsigned pulse;
    int rc;
    reset_diag();
    test_sda_input(0); /* Once only, while idle: OD output from board init. */
    test_sda_write(1);
    rc = high();
    if (rc == 0 && !test_sda_read()) {
        for (pulse = 0u; pulse < 9u; ++pulse) {
            low();
            test_delay_us(HALF_US);
            rc = high();
            if (rc != 0) break;
            test_delay_us(HALF_US);
            if (test_sda_read()) break;
        }
        s_diag.stop_rc = end();
        if (s_diag.stop_rc != 0) rc = TEST_I2C_BUS;
    }
    s_diag.rc = rc;
    s_diag.lines = ack_port_sample();
    return rc;
}

int test_i2c_probe(uint8_t addr7)
{
    int rc;
    reset_diag();
    if (addr7 > 0x7Fu) return s_diag.rc = TEST_I2C_ARGUMENT;
    s_diag.stage = TEST_STAGE_START;
    rc = begin();
    if (rc == 0) { s_diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    return finish(rc);
}

int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value,
                       uint8_t *payload_index)
{
    uint8_t data[3];
    unsigned i;
    int rc;
    reset_diag();
    if (payload_index != 0) *payload_index = 255u;
    if (addr7 > 0x7Fu) return s_diag.rc = TEST_I2C_ARGUMENT;
    data[0] = reg; data[1] = (uint8_t)(value >> 8); data[2] = (uint8_t)value;
    s_diag.stage = TEST_STAGE_START;
    rc = begin();
    if (rc == 0) { s_diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    for (i = 0u; rc == 0 && i < 3u; ++i) {
        s_diag.stage = TEST_STAGE_REG;
        if (payload_index != 0) *payload_index = (uint8_t)i;
        rc = transmit(data[i]);
    }
    return finish(rc);
}

int test_i2c_read_reg(uint8_t addr7, uint8_t reg, uint16_t *value)
{
    uint8_t msb = 0u, lsb = 0u;
    int rc;
    reset_diag();
    if (addr7 > 0x7Fu || value == 0) return s_diag.rc = TEST_I2C_ARGUMENT;
    *value = 0u;
    s_diag.stage = TEST_STAGE_START;
    rc = begin();
    if (rc == 0) { s_diag.stage = TEST_STAGE_ADDR_W; rc = transmit((uint8_t)(addr7 << 1)); }
    if (rc == 0) { s_diag.stage = TEST_STAGE_REG; rc = transmit(reg); }
    if (rc == 0) { s_diag.stage = TEST_STAGE_RESTART; rc = begin(); }
    if (rc == 0) { s_diag.stage = TEST_STAGE_ADDR_R; rc = transmit((uint8_t)((addr7 << 1) | 1u)); }
    if (rc == 0) { s_diag.stage = TEST_STAGE_READ_MSB; rc = receive(&msb, 0); }
    if (rc == 0) { s_diag.stage = TEST_STAGE_READ_LSB; rc = receive(&lsb, 1); }
    rc = finish(rc);
    if (rc == 0) *value = (uint16_t)(((uint16_t)msb << 8) | lsb);
    return rc;
}

void test_i2c_get_diag(test_i2c_diag_t *out) { if (out != 0) *out = s_diag; }
const char *test_i2c_stage_name(test_i2c_stage_t stage)
{
    switch (stage) {
    case TEST_STAGE_START: return "START";
    case TEST_STAGE_ADDR_W: return "ADDR_W";
    case TEST_STAGE_REG: return "REG";
    case TEST_STAGE_RESTART: return "RESTART";
    case TEST_STAGE_ADDR_R: return "ADDR_R";
    case TEST_STAGE_READ_MSB: return "READ_MSB";
    case TEST_STAGE_READ_LSB: return "READ_LSB";
    case TEST_STAGE_STOP: return "STOP";
    case TEST_STAGE_DONE: return "DONE";
    default: return "IDLE";
    }
}
#endif

void ack_i2c_get_trace(volatile ack_trace_t *out)
{
    if (out != 0) *out = trace;
}
