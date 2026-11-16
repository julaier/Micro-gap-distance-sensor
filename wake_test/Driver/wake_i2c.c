/* Compile the UNCHANGED standalone test primitives exactly once.
 * Do not add test_i2c.c as a separate source to this target.
 * Neither the production hard_i2c.c nor the production LDC driver is used. */
#include "wake_i2c.h"
#include "../../test/Driver/test_i2c.c"

int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value,
                       uint8_t *payload_index)
{
    uint8_t payload[3];
    unsigned i;
    int rc;

    if (payload_index != 0) *payload_index = 255u;
    reset_diag();
    if (addr7 > 0x7Fu) return s_diag.rc = TEST_I2C_ARGUMENT;
    payload[0] = reg;
    payload[1] = (uint8_t)(value >> 8);
    payload[2] = (uint8_t)value;
    s_diag.stage = TEST_STAGE_START;
    rc = start();
    if (rc == 0) {
        s_diag.stage = TEST_STAGE_ADDR_W;
        rc = write_byte((uint8_t)(addr7 << 1));
    }
    for (i = 0u; rc == 0 && i < 3u; ++i) {
        s_diag.stage = TEST_STAGE_REG;
        if (payload_index != 0) *payload_index = (uint8_t)i;
        rc = write_byte(payload[i]);
    }
    return finish(rc);
}
