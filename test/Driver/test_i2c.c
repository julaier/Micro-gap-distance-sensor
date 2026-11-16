/* Independent, traditional bit-banged I2C. No old hard_i2c.c dependency.
 * PB7=SCL, PB6=SDA. Every half cycle waits at least 5us, plus CPU overhead.
 * SDA becomes an explicit INPUT during slave ACK and data reception.
 * No UART, calibration, mapping matrix, interrupts or register configuration
 * in this driver. Clock estimates are calculated only after lowering SCL.
 */
#include "test_i2c.h"
#include "test_board.h"

#define HALF_US 5u
#define TIMEOUT_US 2000u

static test_i2c_diag_t s_diag;

static void reset_diag(void)
{
    s_diag.rc = 0;
    s_diag.stop_rc = 0;
    s_diag.stage = TEST_STAGE_IDLE;
    s_diag.tx = 0u;
    s_diag.ack_level = 255u;
    s_diag.lines = test_bus_lines();
    s_diag.clocks = 0u;
    s_diag.hz = 0u;
}

static int scl_high(void)
{
    uint16_t start;
    test_scl_write(1);
    start = test_time_us();
    while (!test_scl_read()) {
        if ((uint16_t)(test_time_us() - start) >= TIMEOUT_US) return TEST_I2C_BUS;
    }
    return TEST_I2C_OK;
}

static int stop(void)
{
    int rc;
    test_scl_write(0);
    test_sda_input(0);
    test_sda_write(0);
    test_delay_us(HALF_US);
    rc = scl_high();
    test_delay_us(HALF_US);
    test_sda_write(1);
    test_delay_us(HALF_US);
    if (rc != 0 || !test_sda_read()) return TEST_I2C_BUS;
    return TEST_I2C_OK;
}

static int start(void)
{
    test_sda_input(0);
    test_sda_write(1);
    test_delay_us(HALF_US);
    if (scl_high() != 0) return TEST_I2C_BUS;
    test_delay_us(HALF_US);
    if (!test_sda_read()) return TEST_I2C_BUS;
    test_sda_write(0);
    test_delay_us(HALF_US);
    test_scl_write(0);
    test_delay_us(HALF_US);
    return TEST_I2C_OK;
}

static int write_byte(uint8_t byte)
{
    unsigned bit;
    uint16_t first_rise = 0u, last_rise, elapsed;
    int rc;

    s_diag.tx = byte;
    s_diag.ack_level = 255u;
    s_diag.clocks = 0u;
    s_diag.hz = 0u;
    for (bit = 0u; bit < 8u; ++bit) {
        test_sda_write((byte & 0x80u) != 0u);
        test_delay_us(HALF_US);
        if (scl_high() != 0) return TEST_I2C_BUS;
        if (bit == 0u) first_rise = test_time_us();
        ++s_diag.clocks;
        test_delay_us(HALF_US);
        test_scl_write(0);
        byte = (uint8_t)(byte << 1);
    }

    test_sda_write(1);
    test_sda_input(1); /* Release SDA before the ninth clock, while SCL is low. */
    test_delay_us(HALF_US);
    if (scl_high() != 0) {
        test_scl_write(0);
        test_sda_input(0);
        return TEST_I2C_BUS;
    }
    last_rise = test_time_us();
    ++s_diag.clocks;
    test_delay_us(HALF_US);
    s_diag.ack_level = test_sda_read() ? 1u : 0u;
    rc = s_diag.ack_level == 0u ? TEST_I2C_OK : TEST_I2C_NACK;
    test_scl_write(0);
    test_delay_us(HALF_US);
    test_sda_input(0);
    elapsed = (uint16_t)(last_rise - first_rise);
    if (elapsed != 0u) s_diag.hz = 8000000u / elapsed;
    return rc;
}

static int read_byte(uint8_t *out, int last)
{
    unsigned bit;
    uint8_t byte = 0u;

    test_sda_input(1);
    for (bit = 0u; bit < 8u; ++bit) {
        test_delay_us(HALF_US);
        if (scl_high() != 0) {
            test_scl_write(0);
            test_sda_input(0);
            return TEST_I2C_BUS;
        }
        test_delay_us(HALF_US);
        byte = (uint8_t)((byte << 1) | (test_sda_read() ? 1u : 0u));
        test_scl_write(0);
    }
    test_sda_input(0);
    test_sda_write(last != 0); /* ACK the MSB, NACK the final LSB. */
    test_delay_us(HALF_US);
    if (scl_high() != 0) {
        test_scl_write(0);
        test_sda_write(1);
        return TEST_I2C_BUS;
    }
    test_delay_us(HALF_US);
    test_scl_write(0);
    test_delay_us(HALF_US);
    test_sda_write(1);
    *out = byte;
    return TEST_I2C_OK;
}

static int finish(int rc)
{
    /* Keep the first failing stage and its pre-STOP line levels. */
    s_diag.lines = test_bus_lines();
    s_diag.stop_rc = stop();
    if (rc == 0 && s_diag.stop_rc != 0) {
        rc = TEST_I2C_BUS;
        s_diag.stage = TEST_STAGE_STOP;
        s_diag.lines = test_bus_lines();
    } else if (rc == 0) {
        s_diag.stage = TEST_STAGE_DONE;
        s_diag.lines = test_bus_lines();
    }
    s_diag.rc = rc;
    return rc;
}

int test_i2c_init(void)
{
    unsigned pulse;
    int rc = TEST_I2C_OK;

    reset_diag();
    test_sda_input(0);
    test_sda_write(1);
    if (scl_high() != 0) rc = TEST_I2C_BUS;
    if (rc == 0 && !test_sda_read()) {
        for (pulse = 0u; pulse < 9u; ++pulse) {
            test_scl_write(0);
            test_delay_us(HALF_US);
            if (scl_high() != 0) { rc = TEST_I2C_BUS; break; }
            test_delay_us(HALF_US);
            if (test_sda_read()) break;
        }
        s_diag.stop_rc = stop();
        if (s_diag.stop_rc != 0) rc = TEST_I2C_BUS;
    }
    s_diag.rc = rc;
    s_diag.lines = test_bus_lines();
    return rc;
}

int test_i2c_probe(uint8_t addr7)
{
    int rc;
    reset_diag();
    if (addr7 > 0x7Fu) return s_diag.rc = TEST_I2C_ARGUMENT;
    s_diag.stage = TEST_STAGE_START;
    rc = start();
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_ADDR_W;
        rc = write_byte((uint8_t)(addr7 << 1));
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
    rc = start();
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_ADDR_W;
        rc = write_byte((uint8_t)(addr7 << 1));
    }
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_REG;
        rc = write_byte(reg);
    }
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_RESTART;
        rc = start();
    }
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_ADDR_R;
        rc = write_byte((uint8_t)((addr7 << 1) | 1u));
    }
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_READ_MSB;
        rc = read_byte(&msb, 0);
    }
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_READ_LSB;
        rc = read_byte(&lsb, 1);
    }
    rc = finish(rc);
    if (rc == 0) *value = (uint16_t)(((uint16_t)msb << 8) | lsb);
    return rc;
}

void test_i2c_get_diag(test_i2c_diag_t *out)
{
    if (out != 0) *out = s_diag;
}

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
