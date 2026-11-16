/*
 * V9: compile the proven standalone I2C source without editing it.
 * This translation unit adds only the legacy application API and register
 * writes. Do NOT also compile test_i2c.c separately in this target.
 */
#include "hard_i2c.h"
#include "../test/Driver/test_i2c.c"

#if HARD_I2C_DIAGNOSTIC_MATRIX
#error V9 uses the original test source with fixed PB7 SCL and PB6 SDA
#endif

static uint16_t s_write_index;

void hard_i2c_init(void)
{
    /* test_board_init() already configured GPIO, USART2 and TIM14. */
    s_write_index = 0u;
    (void)test_i2c_init();
}

void hard_i2c_get_diag(hard_i2c_diag_t *out)
{
    hard_i2c_diag_t d = {0};
    test_i2c_diag_t original;
    if (out == 0) return;
    test_i2c_get_diag(&original);
    d.rc = original.rc;
    d.stop_rc = original.stop_rc;
    switch (original.stage) {
    case TEST_STAGE_START: d.stage = HARD_I2C_START; break;
    case TEST_STAGE_ADDR_W: d.stage = HARD_I2C_ADDR_W; break;
    case TEST_STAGE_REG: d.stage = HARD_I2C_WRITE; d.byte_index = s_write_index; break;
    case TEST_STAGE_RESTART: d.stage = HARD_I2C_RESTART; break;
    case TEST_STAGE_ADDR_R: d.stage = HARD_I2C_ADDR_R; break;
    case TEST_STAGE_READ_MSB: d.stage = HARD_I2C_READ; break;
    case TEST_STAGE_READ_LSB: d.stage = HARD_I2C_READ; d.byte_index = 1u; break;
    case TEST_STAGE_STOP: d.stage = HARD_I2C_STOP; break;
    case TEST_STAGE_DONE: d.stage = HARD_I2C_DONE; break;
    default: d.stage = HARD_I2C_IDLE; break;
    }
    d.lines = original.lines;
    d.effective_hz = original.hz;
    d.half_ticks = HALF_US;
    d.clocks = original.clocks;
    d.tx_byte = original.tx;
    d.ack_level = original.ack_level;
    /* The unchanged test core does not collect these legacy samples. */
    d.sampled_byte = d.ack_samples = 255u;
    d.ack_sda_mode = d.ack_sda_od = 255u;
    *out = d;
}

int hard_i2c_calibrate_100k(uint8_t addr7)
{
    (void)addr7;
    reset_diag();
    return s_diag.rc = TEST_I2C_ARGUMENT;
}

void hard_i2c_debug_config(uint8_t swapped, uint8_t input_rx)
{
    (void)swapped;
    (void)input_rx;
    /* Compatibility only; never change the proven pin/mode configuration. */
    (void)stop();
}

void hard_i2c_pin_test(uint8_t levels[4])
{
    if (levels == 0) return;
    levels[0] = test_bus_lines();
    test_scl_write(0);
    test_delay_us(HALF_US);
    levels[1] = test_bus_lines();
    test_sda_input(0);
    test_sda_write(0);
    test_delay_us(HALF_US);
    (void)scl_high();
    test_delay_us(HALF_US);
    levels[2] = test_bus_lines();
    test_sda_write(1);
    test_delay_us(HALF_US);
    levels[3] = test_bus_lines();
}

uint8_t hard_i2c_lines(void)
{
    /* Legacy pull-strength test is intentionally not run. 255 = unavailable. */
    return 255u;
}

static int app_begin(uint8_t address, int repeated)
{
    int rc;
    s_diag.stage = repeated ? TEST_STAGE_RESTART : TEST_STAGE_START;
    rc = start();
    if (rc == 0) {
        s_diag.stage = (address & 1u) ? TEST_STAGE_ADDR_R : TEST_STAGE_ADDR_W;
        rc = write_byte(address);
    }
    return rc;
}

int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    int rc;
    s_write_index = 0u;
    /* Address-only transactions use the original public probe verbatim. */
    if (len == 0u) return test_i2c_probe(addr7);
    reset_diag();
    if (addr7 > 0x7Fu || buf == 0 || len > 255u)
        return s_diag.rc = TEST_I2C_ARGUMENT;
    rc = app_begin((uint8_t)(addr7 << 1), 0);
    for (i = 0u; rc == 0 && i < len; ++i) {
        s_diag.stage = TEST_STAGE_REG;
        s_write_index = i;
        rc = write_byte(buf[i]); /* Original test byte/ACK implementation. */
    }
    return finish(rc);
}

int hard_i2c_write_read(uint8_t addr7, const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen)
{
    uint16_t i, value = 0u;
    uint8_t rx[2] = {0u, 0u};
    int rc;
    s_write_index = 0u;
    reset_diag();
    if (addr7 > 0x7Fu || (wbuf == 0 && wlen != 0u) || wlen > 255u ||
        rbuf == 0 || (rlen != 1u && rlen != 2u))
        return s_diag.rc = TEST_I2C_ARGUMENT;

    /* Every LDC1614 ID/configuration/data read takes this exact test path. */
    if (wlen == 1u && rlen == 2u) {
        rc = test_i2c_read_reg(addr7, wbuf[0], &value);
        if (rc == 0) {
            rbuf[0] = (uint8_t)(value >> 8);
            rbuf[1] = (uint8_t)value;
        }
        return rc;
    }

    /* Legacy non-LDC callers, still using only the original primitives. */
    rc = app_begin((uint8_t)(addr7 << 1), 0);
    for (i = 0u; rc == 0 && i < wlen; ++i) {
        s_diag.stage = TEST_STAGE_REG;
        s_write_index = i;
        rc = write_byte(wbuf[i]);
    }
    if (rc == 0) rc = app_begin((uint8_t)((addr7 << 1) | 1u), 1);
    for (i = 0u; rc == 0 && i < rlen; ++i) {
        s_diag.stage = i == 0u ? TEST_STAGE_READ_MSB : TEST_STAGE_READ_LSB;
        rc = read_byte(&rx[i], i + 1u == rlen);
    }
    rc = finish(rc);
    if (rc == 0) {
        for (i = 0u; i < rlen; ++i) rbuf[i] = rx[i];
    }
    return rc;
}
