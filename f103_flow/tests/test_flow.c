/* Transaction-level integration model. The real B GPIO/ACK driver has its
 * own edge-decoding regression suite in ack_compare/tools/run_tests.ps1.
 * Each invocation is a fresh simulated boot; no test-only reset in firmware.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hard_i2c.h"
#include "flow_bus.h"
#include "f103_flow.h"
#include "ldc1614.h"

static uint16_t regs[128];
static int calls, fail_call = -1, init_failure, wrong_id, wake_reset, commit_nack;
static unsigned resets, configs, writes, delay_count, delay_value;
static unsigned output_count, no_output_limit = 15u;
static test_i2c_diag_t diag;
static ack_trace_t trace;
static const uint8_t original_regs[] = {
    0x1C,0x7E,0x7F,0x14,0x15,0x10,0x11,0x08,0x09,0x1E,0x1F,0x1B,0x1A,0x1A,0x1A
};
static const char original_ops[] = "WRRWWWWWWWWWRWR";
static const uint16_t original_values[] = {
    0x8000,0,0,0x1001,0x1001,0x0180,0x0180,0xFFFF,0xFFFF,0x9000,0x9000,0x820C,0,0x0A01,0
};
static const int original_errors[] = {
    -10,-11,-11,-12,-12,-13,-13,-14,-14,-15,-15,-16,-17,-17,-18
};

static void defaults(void)
{
    memset(regs, 0, sizeof(regs));
    regs[0x7E] = wrong_id ? 0x1234u : 0x5449u; regs[0x7F] = 0x3055u;
    regs[0x1A] = 0x2801u; regs[0x1B] = 0x020Fu;
    regs[0x08] = regs[0x09] = 0x0080u;
}
static int transaction(char op, uint8_t reg, uint16_t value)
{
    int index = calls++;
    if ((unsigned)index < no_output_limit) assert(output_count == 0u);
    memset(&diag, 0, sizeof(diag)); memset(&trace, 0, sizeof(trace));
    diag.rc = index == fail_call ? TEST_I2C_NACK : 0;
    diag.stage = diag.rc ? TEST_STAGE_REG : TEST_STAGE_DONE;
    diag.tx = op == 'W' ? (uint8_t)value : reg;
    diag.clocks = 9; diag.hz = 36000; diag.ack_level = diag.rc ? 1u : 0u;
    trace.count = 1; trace.bytes[0].samples = 3;
    trace.bytes[0].scl_mask = 7; trace.bytes[0].sda_mask = diag.rc ? 7u : 0u;
    trace.bytes[0].rc = diag.rc; trace.bytes[0].tx = diag.tx;
    if (op == 'W') {
        ++writes;
        if (diag.rc == 0 || (commit_nack && reg == 0x1A)) {
            if (reg == 0x1C) { assert(value == 0x8000); ++resets; defaults(); }
            else {
                regs[reg] = value;
                if (reg == 0x1A) {
                    assert(value == 0x0A01); ++configs;
                    if (wake_reset) defaults();
                }
            }
        }
    }
    return diag.rc;
}
int test_i2c_init(void) { return init_failure ? TEST_I2C_BUS : 0; }
int test_i2c_probe(uint8_t addr) { assert(addr == 0x2A); return 0; }
int wake_i2c_write_reg(uint8_t addr, uint8_t reg, uint16_t v, uint8_t *idx)
{
    assert(addr == 0x2A); *idx = 2;
    return transaction('W', reg, v);
}
int test_i2c_read_reg(uint8_t addr, uint8_t reg, uint16_t *value)
{
    int rc;
    assert(addr == 0x2A && reg < 128);
    rc = transaction('R', reg, 0);
    if (rc == 0) *value = regs[reg];
    if (rc == 0 && reg == 0x00) regs[0x18] &= (uint16_t)~0x0008u;
    if (rc == 0 && reg == 0x02) regs[0x18] &= (uint16_t)~0x0004u;
    return rc;
}
void test_i2c_get_diag(test_i2c_diag_t *out) { *out = diag; }
void ack_i2c_get_trace(volatile ack_trace_t *out) { *out = trace; }
const char *test_i2c_stage_name(test_i2c_stage_t stage)
{ return stage == TEST_STAGE_DONE ? "DONE" : "REG"; }
uint8_t test_bus_lines(void) { return 3; }
void test_delay_ms(uint16_t ms) { ++delay_count; delay_value = ms; }
void test_uart_putc(char c) { (void)c; ++output_count; }
void test_uart_puts(const char *s) { while (*s) test_uart_putc(*s++); }

static void check_records(unsigned n)
{
    unsigned i;
    assert(g_flow_bus.count == n && !g_flow_bus.overflow && !g_flow_bus.recording);
    for (i = 0; i < n; ++i) {
        const volatile flow_record_t *r = &g_flow_bus.records[i];
        assert(r->operation == original_ops[i] && r->reg == original_regs[i]);
        if (r->operation == 'W') assert(r->value == original_values[i]);
    }
}

int main(int argc, char **argv)
{
    int expected_rc = 0, rc, saved_calls;
    unsigned expected_records = 15u, saved_writes, saved_resets;
    flow_bus_debug_t saved;
    assert(argc == 2);
    if (strcmp(argv[1], "ok") == 0) { }
    else if (strcmp(argv[1], "wrong_id") == 0) {
        wrong_id = 1; expected_rc = -11; expected_records = 3;
    } else if (strcmp(argv[1], "wake_reset") == 0) {
        wake_reset = 1; expected_rc = -18;
    } else if (strcmp(argv[1], "commit_nack") == 0) {
        commit_nack = 1; fail_call = 13; expected_rc = -17; expected_records = 14;
    } else if (strcmp(argv[1], "init_bus") == 0) {
        init_failure = 1; expected_rc = -1; expected_records = 0;
    } else if (strcmp(argv[1], "snapshot_nack") == 0) {
        fail_call = 15; expected_rc = -2;
    } else {
        fail_call = atoi(argv[1]); assert(fail_call >= 0 && fail_call < 15);
        expected_rc = original_errors[fail_call]; expected_records = (unsigned)fail_call + 1u;
    }
    no_output_limit = expected_records;
    defaults();
    rc = f103_flow_start();
    assert(rc == expected_rc && g_flow_debug.result == rc && g_flow_debug.done);
    assert(g_flow_debug.checkpoint == 1);
    check_records(expected_records);
    assert(resets <= 1u && configs <= 1u);
    assert(delay_count == (unsigned)(!init_failure && fail_call != 0));
    if (delay_count) assert(delay_value == 5u); /* No 1-second hold or wake quiet. */
    if (fail_call >= 0 && fail_call < 15) {
        assert(g_flow_bus.first_failed);
        assert(g_flow_bus.first_failure.reg == original_regs[fail_call]);
        assert(g_flow_bus.first_failure.rc == -2);
        assert(g_flow_bus.first_ack.bytes[0].sda_mask == 7);
    } else assert(!g_flow_bus.first_failed);
    saved = g_flow_bus; saved_writes = writes; saved_resets = resets;
    saved_calls = calls;
    assert(f103_flow_start() == TEST_I2C_ARGUMENT && calls == saved_calls);
    f103_flow_monitor(1u);
    assert(writes == saved_writes && resets == saved_resets);
    assert(g_flow_bus.count == saved.count && g_flow_bus.first_failed == saved.first_failed);
    if (saved.first_failed) {
        assert(g_flow_bus.first_failure.reg == saved.first_failure.reg);
        assert(g_flow_bus.first_failure.rc == saved.first_failure.rc);
    }
    if (rc == 0) {
        assert(calls == saved_calls); /* Monitor must not steal unread DATA. */
        f103_flow_poll(); assert(g_flow_debug.poll_rc == 1 && g_flow_debug.new_pairs == 0);
        regs[0x18] = 0x000C; regs[0x00] = 0x0012; regs[0x01] = 0x3456;
        regs[0x02] = 0x0045; regs[0x03] = 0x6789;
        f103_flow_poll();
        assert(g_flow_debug.poll_rc == 0 && g_flow_debug.ch0 == 0x00123456);
        assert(g_flow_debug.ch1 == 0x00456789 && g_flow_debug.valid_pairs == 1);
        assert(g_flow_debug.new_pairs == 1);
        f103_flow_poll(); assert(g_flow_debug.poll_rc == 1 && g_flow_debug.new_pairs == 1);
        regs[0x18] = 0x000C; regs[0x00] |= 0x1000;
        f103_flow_poll(); assert(g_flow_debug.poll_rc == 2);
        assert(g_flow_debug.data_errors == 0x1000 && g_flow_debug.valid_pairs == 1);
        regs[0x18] = 0x000C; regs[0x00] = regs[0x01] = 0;
        f103_flow_poll(); assert(g_flow_debug.poll_rc == 2 && g_flow_debug.valid_pairs == 1);
        fail_call = calls;
        f103_flow_poll(); assert(g_flow_debug.poll_rc == -2);
        assert(!g_flow_bus.first_failed && g_flow_bus.count == 15);
    } else {
        saved_calls = calls; f103_flow_poll(); assert(calls == saved_calls);
    }
    {
        uint8_t w = 0x1A, r[2] = {0xAA,0xBB};
        int n = calls;
        assert(hard_i2c_write(0x80, 0, 0) == TEST_I2C_ARGUMENT);
        assert(hard_i2c_write(0x2A, 0, 3) == TEST_I2C_ARGUMENT);
        assert(hard_i2c_write_read(0x2A, &w, 1, r, 1) == TEST_I2C_ARGUMENT);
        assert(calls == n);
        fail_call = calls;
        assert(hard_i2c_write_read(0x2A, &w, 1, r, 2) == -2);
        assert(r[0] == 0xAA && r[1] == 0xBB); /* No fake data after NACK. */
    }
    printf("PASS F103 flow: %s\n", argv[1]);
    return 0;
}
