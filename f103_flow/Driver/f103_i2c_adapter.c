/* Implements exactly the transaction shapes used by the original LDC driver.
 * Do not link new/Driver/hard_i2c.c (F103 hardware registers) into this target.
 * Never retry, ignore NACK, reset the LDC, or recover after a transaction error.
 */
#include "hard_i2c.h"
#include "flow_bus.h"
#include "test_board.h"

volatile flow_bus_debug_t g_flow_bus;
static unsigned initialized;

static void remember(uint8_t operation, uint8_t reg, uint16_t value,
                     uint8_t index, int rc)
{
    flow_record_t r;
    if (!g_flow_bus.recording) return;
    r.operation = operation; r.reg = reg; r.value = value;
    r.payload_index = index; r.rc = rc;
    test_i2c_get_diag(&r.bus);
    if (g_flow_bus.count < FLOW_RECORDS)
        g_flow_bus.records[g_flow_bus.count++] = r;
    else g_flow_bus.overflow = 1u;
    if (rc != 0 && !g_flow_bus.first_failed) {
        g_flow_bus.first_failure = r;
        ack_i2c_get_trace(&g_flow_bus.first_ack);
        g_flow_bus.first_failed = 1u;
    }
}

void hard_i2c_init(void)
{
    if (initialized) return;
    initialized = 1u;
    g_flow_bus.recording = 1u;
    /* Same single initial bus-release check as B. No later recovery calls. */
    g_flow_bus.init_rc = test_i2c_init();
}

void flow_bus_freeze(void) { g_flow_bus.recording = 0u; }
uint8_t hard_i2c_lines(void) { return test_bus_lines(); }

int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
    uint8_t index = 255u;
    uint16_t value;
    int rc;
    if (addr7 > 0x7Fu) return TEST_I2C_ARGUMENT;
    if (len == 0u) return test_i2c_probe(addr7);
    if (buf == 0 || len != 3u) return TEST_I2C_ARGUMENT;
    value = (uint16_t)(((uint16_t)buf[1] << 8) | buf[2]);
    rc = wake_i2c_write_reg(addr7, buf[0], value, &index);
    remember('W', buf[0], value, index, rc);
    return rc;
}

int hard_i2c_write_read(uint8_t addr7, const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen)
{
    uint16_t value = 0u;
    int rc;
    if (addr7 > 0x7Fu || wbuf == 0 || rbuf == 0 || wlen != 1u || rlen != 2u)
        return TEST_I2C_ARGUMENT;
    rc = test_i2c_read_reg(addr7, wbuf[0], &value);
    if (rc == 0) { rbuf[0] = (uint8_t)(value >> 8); rbuf[1] = (uint8_t)value; }
    remember('R', wbuf[0], value, 255u, rc);
    return rc;
}
