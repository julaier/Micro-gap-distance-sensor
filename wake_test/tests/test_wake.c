/* An edge-decoding slave, not scripted ACK samples. The actual unchanged
 * test_i2c.c primitives and the new write adapter execute against this model.
 * Include the real diagnostic module so each scenario can reset its BSS as
 * a separate MCU boot would. No production application source is included. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_board.h"
#include "wake_i2c.h"
#include "../App/wake_diag.c"

enum { RX_BITS, RX_ACK, TX_BITS, TX_ACK, WAIT_STOP };
enum { OK, WAKE_NACK, WAKE_RESET_NACK, CORRUPT_RCOUNT, WAKE_READ_NACK,
       DELAYED_RESET, WRONG_ID, NO_TARGET, SETUP_NACK, WAKE_COMMIT_NACK,
       SLEEP_READ_NACK, DRIVE_FIELD_CORRUPT, WAKE_STOP_FAIL,
       SLEEP_WRITE_NACK, SLEEP_RESET_ON_WAIT, SLEEP_RCOUNT_LOSS,
       SLEEP_POSTWAIT_NACK, SLEEP_NOW_NACK, INIT_DRIVE_CHANGED,
       SLEEP_COMMIT_NACK, UNEXPECTED_RESET_CONFIG, SLEEP_DATA_NACK,
       SLEEP_FIELD_LOSS, SLEEP_LATE_FIELD_LOSS,
       EARLY_READ_RESET, QUIET_RESET, WAKE_MSB_NACK, WAKE_MSB_RESET };
static unsigned phase, bits, byte, address_byte, selected, reading, ack_wanted;
static unsigned active, ack_seen, read_index, pointer, payload_count, high_byte;
static unsigned starts, stops, master_ack_count, master_ack_phase;
static unsigned addr_count, addr_bytes[2048], slave_address;
static uint16_t registers[128];
static unsigned written_regs[128], written_values[128], written_count;
static unsigned explicit_resets, fault, wake_seen, delay_count, delays[8];
static unsigned sleep_seen, held_wait_done, written_at_sleep_wait, monitor_fault;
static unsigned lost_register;
static int scl_release, sda_release, sda_input_mode, slave_low;
static int force_scl_low, force_sda_low, hold_stop, nack_payload;
static uint32_t ticks;
static char output[65536];
static unsigned output_count, output_at_first_config, output_at_wake;
static unsigned quiet_wait_seen, wake_stop_seen, first_wake_read_seen;
static unsigned wake_msb_attempted;
static uint32_t wake_stop_tick, first_wake_read_tick;

static void reset_registers(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0x7E] = fault == WRONG_ID ? 0x1234u : 0x5449u;
    registers[0x7F] = 0x3055u;
    registers[0x1A] = fault == UNEXPECTED_RESET_CONFIG ? 0x0801u : 0x2801u;
    registers[0x1B] = 0x020Fu;
    registers[0x08] = registers[0x09] = 0x0080u;
}

static unsigned bus(void)
{
    return (scl_release && !force_scl_low ? 1u : 0u) |
           ((sda_release || sda_input_mode) && !slave_low && !force_sda_low ? 2u : 0u);
}

static unsigned tx_value(void)
{
    uint16_t value = registers[pointer & 127u];
    return (read_index & 1u) == 0u ? value >> 8 : value & 255u;
}

static void set_tx_bit(void) { slave_low = (tx_value() & (0x80u >> bits)) == 0u; }

static void commit(unsigned value)
{
    int suppress = 0;
    assert(written_count < 128u && pointer < 128u);
    written_regs[written_count] = pointer;
    written_values[written_count++] = value;
    if (pointer == 0x14u && output_at_first_config == 0u)
        output_at_first_config = output_count;
    if (pointer == 0x1Cu) {
        assert(value == 0x8000u);
        ++explicit_resets;
        reset_registers();
        return;
    }
    if (pointer == 0x1Au) {
        if (value & 0x2000u) {
            assert(value == 0x3401u);
            ++sleep_seen;
            if (fault == SLEEP_WRITE_NACK || fault == SLEEP_COMMIT_NACK)
                ack_wanted = 0u;
            if (fault != SLEEP_WRITE_NACK) registers[pointer] = (uint16_t)value;
            if (fault == SLEEP_RCOUNT_LOSS) registers[0x09] = 0x0080u;
            if (fault == SLEEP_FIELD_LOSS)
                registers[lost_register] ^= lost_register >= 0x1Eu ? 0x0800u : 1u;
            return;
        }
        /* Full-current activation (bit11=0), only bit13 changes from 3401,
         * and never wake until both sleeping snapshots have passed. */
        assert(value == 0x1401u && sleep_seen == 1u && held_wait_done);
        assert(g_wake_debug.phase == WAKE_DEBUG_WAKE_WRITE);
        assert(g_wake_debug.checkpoint == 1u);
        ++wake_seen;
        output_at_wake = output_count;
        if (fault == WAKE_NACK || fault == WAKE_RESET_NACK || fault == WAKE_COMMIT_NACK)
            ack_wanted = 0u;
        if (fault == WAKE_NACK || fault == WAKE_RESET_NACK) suppress = 1;
        if (fault == WAKE_RESET_NACK) reset_registers();
        if (fault == CORRUPT_RCOUNT) registers[0x08] = 0x0080u;
        if (fault == DRIVE_FIELD_CORRUPT) registers[0x1E] = registers[0x1F] = 0x7800u;
        if (fault == INIT_DRIVE_CHANGED) registers[0x1E] = registers[0x1F] = 0x9400u;
        if (fault == WAKE_STOP_FAIL) hold_stop = 1;
    }
    if (!suppress) registers[pointer] = (uint16_t)value;
}

static void rising(void)
{
    unsigned level = (bus() & 2u) != 0u;
    /* Stuck-low fault after STOP prevents meaningful further frame decoding. */
    if (!active || force_sda_low) return;
    switch (phase) {
    case RX_BITS:
        byte = (byte << 1) | level;
        if (++bits == 8u) {
            if (address_byte) {
                assert(addr_count < 2048u);
                addr_bytes[addr_count++] = byte;
                reading = byte & 1u;
                selected = (byte >> 1) == slave_address;
                ack_wanted = selected;
                if (reading && ((fault == WAKE_READ_NACK && wake_seen) ||
                    (fault == SLEEP_READ_NACK && pointer == 0x1Au))) ack_wanted = 0u;
                if (reading && sleep_seen && !wake_seen &&
                    ((fault == SLEEP_NOW_NACK && pointer == 0x11u) ||
                     (fault == SLEEP_POSTWAIT_NACK && held_wait_done && pointer == 0x11u) ||
                     (fault == SLEEP_DATA_NACK && pointer == 0u))) ack_wanted = 0u;
                if (reading && monitor_fault && pointer == 0x11u) ack_wanted = 0u;
            } else {
                assert(selected);
                ack_wanted = (int)payload_count != nack_payload;
                if (payload_count == 1u && pointer == 0x1Au && byte == 0x14u) {
                    ++wake_msb_attempted;
                    output_at_wake = output_count;
                    if (fault == WAKE_MSB_NACK || fault == WAKE_MSB_RESET)
                        ack_wanted = 0u;
                    if (fault == WAKE_MSB_RESET) reset_registers();
                }
                if (fault == SETUP_NACK && pointer == 0x10u && payload_count == 1u)
                    ack_wanted = 0u;
                if (ack_wanted) {
                    if (payload_count == 0u) pointer = byte;
                    else if (payload_count == 1u) high_byte = byte;
                    else if (payload_count == 2u) commit((high_byte << 8) | byte);
                    else assert(!"Too many write payload bytes");
                }
                ++payload_count;
            }
            phase = RX_ACK;
            ack_seen = 0u;
        }
        break;
    case RX_ACK:
        assert(sda_input_mode && level == (ack_wanted ? 0u : 1u));
        ack_seen = 1u;
        break;
    case TX_BITS:
        assert(sda_input_mode);
        if (++bits == 8u) { phase = TX_ACK; ack_seen = 0u; }
        break;
    case TX_ACK:
        assert(!sda_input_mode);
        assert(level == master_ack_phase); /* ACK first byte, NACK second. */
        master_ack_phase ^= 1u;
        ++master_ack_count;
        ack_seen = 1u;
        break;
    default: break;
    }
}

static void falling(void)
{
    if (!active || force_sda_low) return;
    switch (phase) {
    case RX_ACK:
        if (!ack_seen) slave_low = (int)ack_wanted;
        else {
            slave_low = 0;
            bits = byte = 0u;
            if (!ack_wanted) phase = WAIT_STOP;
            else if (address_byte && reading) {
                phase = TX_BITS;
                read_index = master_ack_phase = 0u;
                set_tx_bit();
            } else phase = RX_BITS;
            address_byte = 0u;
        }
        break;
    case TX_BITS: set_tx_bit(); break;
    case TX_ACK:
        if (!ack_seen) slave_low = 0;
        else if (master_ack_phase == 0u) phase = WAIT_STOP;
        else {
            ++read_index;
            bits = 0u;
            phase = TX_BITS;
            set_tx_bit();
        }
        break;
    default: break;
    }
}

static void changed(unsigned before)
{
    unsigned after = bus();
    if ((before & 1u) && (after & 1u) && ((before ^ after) & 2u)) {
        if (after & 2u) {
            ++stops; active = 0u; slave_low = 0;
            if (wake_msb_attempted && !wake_stop_seen) {
                wake_stop_seen = 1u;
                wake_stop_tick = ticks;
            }
        }
        else {
            if (wake_msb_attempted && !first_wake_read_seen) {
                first_wake_read_seen = 1u;
                first_wake_read_tick = ticks;
                if (fault == EARLY_READ_RESET && ticks - wake_stop_tick < 5000u)
                    reset_registers();
            }
            ++starts;
            active = address_byte = 1u;
            selected = reading = ack_wanted = 0u;
            bits = byte = ack_seen = payload_count = 0u;
            phase = RX_BITS;
        }
    }
    if (!(before & 1u) && (after & 1u)) rising();
    if ((before & 1u) && !(after & 1u)) falling();
}

void test_scl_write(int release)
{
    unsigned before = bus();
    scl_release = release;
    changed(before);
}
void test_sda_write(int release)
{
    unsigned before = bus();
    if (release && hold_stop && scl_release) force_sda_low = 1;
    sda_release = release;
    changed(before);
}
void test_sda_input(int input)
{
    unsigned before = bus();
    sda_release = 1;
    sda_input_mode = input;
    changed(before);
}
int test_scl_read(void) { return (bus() & 1u) != 0u; }
int test_sda_read(void) { return (bus() & 2u) != 0u; }
uint8_t test_bus_lines(void) { return (uint8_t)bus(); }
uint16_t test_time_us(void) { return (uint16_t)++ticks; }
void test_delay_us(uint16_t us) { ticks += us; }
void test_delay_ms(uint16_t ms)
{
    assert(delay_count < 8u);
    delays[delay_count++] = ms;
    if (wake_msb_attempted && ms == 5u) {
        /* The requested quiet interval precedes ALL post-wake traffic/UART.
         * STOP faults are still reported, never silently repaired. */
        assert(!first_wake_read_seen && output_count == output_at_wake);
        assert(g_wake_debug.phase == WAKE_DEBUG_QUIET);
        assert(g_wake_debug.wake_write_rc == writes[11].rc);
        assert(written_count ==
            (fault == WAKE_MSB_NACK || fault == WAKE_MSB_RESET ? 11u : 12u));
        if (fault != WAKE_STOP_FAIL) assert(!active && bus() == 3u);
        ++quiet_wait_seen;
        if (fault == QUIET_RESET) reset_registers();
    }
    ticks += (uint32_t)ms * 1000u;
    if (fault == DELAYED_RESET && ms == 20u) reset_registers();
    if (ms == 1000u) {
        assert(sleep_seen == 1u && wake_seen == 0u);
        held_wait_done = 1u;
        written_at_sleep_wait = written_count;
        if (fault == SLEEP_RESET_ON_WAIT) reset_registers();
        if (fault == SLEEP_LATE_FIELD_LOSS)
            registers[lost_register] ^= lost_register >= 0x1Eu ? 0x0800u : 1u;
    }
}
int test_intb_read(void) { return 1; }
void test_uart_putc(char c)
{
    assert(output_count + 1u < sizeof(output));
    output[output_count++] = c;
    output[output_count] = '\0';
}
void test_uart_puts(const char *s) { while (*s) test_uart_putc(*s++); }

static void reset_model(unsigned mode)
{
    phase = RX_BITS;
    bits = byte = address_byte = selected = reading = ack_wanted = 0u;
    active = ack_seen = read_index = pointer = payload_count = high_byte = 0u;
    starts = stops = master_ack_count = master_ack_phase = addr_count = 0u;
    written_count = explicit_resets = wake_seen = delay_count = 0u;
    sleep_seen = held_wait_done = written_at_sleep_wait = monitor_fault = 0u;
    lost_register = 0u;
    scl_release = sda_release = 1;
    sda_input_mode = slave_low = force_scl_low = force_sda_low = hold_stop = 0;
    nack_payload = -1;
    ticks = output_count = output_at_first_config = output_at_wake = 0u;
    quiet_wait_seen = wake_stop_seen = first_wake_read_seen = 0u;
    wake_msb_attempted = 0u;
    wake_stop_tick = first_wake_read_tick = 0u;
    output[0] = '\0';
    fault = mode;
    slave_address = mode == NO_TARGET ? 0x2Bu : 0x2Au;
    reset_registers();
    /* Reset only this diagnostic module's RAM, as on a new MCU boot. */
    attempted = configured = 0u;
    write_count = 0u;
    target_config = 0x3401u;
    memset((void *)&g_wake_debug, 0, sizeof(g_wake_debug));
    g_wake_debug.mcu_reset_flags = 0x04000000u;
    g_wake_quiet = 1u;
}

static void check_read_only_monitor(void)
{
    unsigned count = written_count, resets = explicit_resets, wakes = wake_seen;
    wake_debug_t saved = g_wake_debug;
    unsigned i;
    for (i = 1u; i <= 5u; ++i) wake_diag_monitor(i);
    assert(written_count == count && explicit_resets == resets && wake_seen == wakes);
    assert(wake_diag_start() == TEST_I2C_ARGUMENT);
    assert(written_count == count && explicit_resets == resets && wake_seen == wakes);
    assert(strstr(output, "$B,RESTART_BLOCKED,") != NULL);
    assert(memcmp((const void *)&g_wake_debug, &saved, sizeof(saved)) == 0);
}

static void test_adapter(void)
{
    uint8_t index;
    test_i2c_diag_t d;
    int i;
    reset_model(OK);
    assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x1234u, &index) == 0);
    assert(index == 2u && registers[0x08] == 0x1234u);
    assert(addr_count == 1u && addr_bytes[0] == 0x54u);
    assert(written_count == 1u && written_regs[0] == 0x08u && written_values[0] == 0x1234u);
    assert(starts == 1u && stops == 1u && bus() == 3u && !active);
    test_i2c_get_diag(&d);
    assert(d.stage == TEST_STAGE_DONE && d.clocks == 9u && d.stop_rc == 0);
    for (i = 0; i < 3; ++i) {
        reset_model(OK);
        nack_payload = i;
        assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x1234u, &index) == -2);
        test_i2c_get_diag(&d);
        assert(index == (unsigned)i && d.stage == TEST_STAGE_REG && d.ack_level == 1u);
        assert(written_count == 0u && stops == 1u && bus() == 3u);
    }
    reset_model(NO_TARGET);
    assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x1234u, &index) == -2 && index == 255u);
    assert(written_count == 0u);
    reset_model(OK);
    assert(wake_i2c_write_reg(0x80u, 0x08u, 0x1234u, &index) == -3 && starts == 0u);
    ticks = 65520u;
    assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x5678u, 0) == 0);
    assert(registers[0x08] == 0x5678u);
    reset_model(OK);
    force_scl_low = 1;
    assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x1234u, &index) == -1);
    assert(index == 255u && ticks < 5000u && written_count == 0u);
}

int main(void)
{
    static const unsigned expected_regs[] = {0x1C,0x14,0x15,0x10,0x11,0x08,0x09,0x1E,0x1F,0x1B,0x1A,0x1A};
    static const unsigned expected_values[] = {0x8000,0x1001,0x1001,0x0180,0x0180,0xFFFF,0xFFFF,0x9000,0x9000,0x820C,0x3401,0x1401};
    unsigned i, mode;
    int rc;
    test_adapter();
    /* Exactly one CONFIG bit differs from V4: LDC reference select bit9.
     * Keep manual drive, full-current activation and the separate bit6. */
    assert((CONFIG_MANUAL_AWAKE ^ 0x1601u) == 0x0200u);
    assert((CONFIG_MANUAL_SLEEP ^ 0x3601u) == 0x0200u);
    assert((CONFIG_MANUAL_AWAKE & 0x0200u) == 0u);
    assert((CONFIG_MANUAL_AWAKE & 0x1400u) == 0x1400u);
    assert((CONFIG_MANUAL_AWAKE & 0x0800u) == 0u);
    assert((CONFIG_MANUAL_SLEEP ^ CONFIG_MANUAL_AWAKE) == 0x2000u);
    assert((CONFIG_MANUAL_AWAKE & 0x0040u) == 0u);
    reset_model(OK);
    assert(wake_diag_start() == 0);
    assert(written_count == 12u && explicit_resets == 1u && sleep_seen == 1u && wake_seen == 1u);
    for (i = 0u; i < 12u; ++i) {
        assert(written_regs[i] == expected_regs[i] && written_values[i] == expected_values[i]);
    }
    assert(delay_count == 4u && delays[0] == 5u && delays[1] == 1000u &&
           delays[2] == 5u && delays[3] == 20u);
    assert(quiet_wait_seen == 1u && first_wake_read_tick - wake_stop_tick >= 5000u);
    assert(g_wake_debug.magic == 0x57414B35u && g_wake_debug.mcu_reset_flags == 0x04000000u);
    assert(g_wake_debug.done == 1u && g_wake_debug.phase == WAKE_DEBUG_DONE);
    assert(g_wake_debug.checkpoint == 2u && g_wake_debug.result == 0);
    assert(g_wake_debug.before_config == 0x2801u && g_wake_debug.after_config == 0x1401u);
    assert(g_wake_debug.wake_write_rc == 0 && g_wake_debug.wake_read_rc == 0);
    assert(g_wake_debug.sleep_now.hold == 0 && g_wake_debug.sleep_1s.hold == 0);
    assert(g_wake_debug.sleep_now.captured && g_wake_debug.sleep_1s.captured);
    assert(g_wake_debug.first_reads.captured && g_wake_debug.plus20ms.captured);
    assert(g_wake_debug.first_reads.comm == 0 && g_wake_debug.plus20ms.hold == 0);
    assert(strstr(output, "$B,WAKE_QUIET_MS,5\r\n") != NULL);
    assert(written_at_sleep_wait == 11u);
    assert(output_at_first_config != 0u && output_at_first_config == output_at_wake);
    assert(strstr(output, "$S,1A,3401,0,0,3401\r\n") != NULL);
    assert(strstr(output, "$Q,SLEEP_NOW,1A,0,3401\r\n") != NULL);
    assert(strstr(output, "$Q,SLEEP_1S,1A,0,3401\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_NOW,COMM,0\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_NOW,HOLD,0\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_1S,HOLD,0\r\n") != NULL);
    assert(strstr(output, "$C,1A,1401,0,0,1401\r\n") != NULL);
    assert(strstr(output, "$K,FIRST_READS,HOLD,0\r\n") != NULL);
    assert(strstr(output, "$K,PLUS20MS,HOLD,0\r\n") != NULL);
    /* No coils/data model: zero data must not be reported as measurement ready. */
    assert(strstr(output, "$B,RESULT,CONFIG_PASS_NOT_MEASUREMENT,0\r\n") != NULL);
    assert(strstr(output, "$Q,FIRST_READS,00,0,0000\r\n") != NULL);
    /* Mixed failures used to be hidden by the first -18 summary. */
    registers[0x1A] = 0x2801u;
    monitor_fault = 1u;
    wake_diag_monitor(1u);
    assert(strstr(output, "$K,MONITOR,COMM,-2\r\n") != NULL);
    assert(strstr(output, "$K,MONITOR,HOLD,-18\r\n") != NULL);
    monitor_fault = 0u;
    check_read_only_monitor();

    for (mode = WAKE_NACK; mode <= SLEEP_DATA_NACK; ++mode) {
        reset_model(mode);
        rc = wake_diag_start();
        if (mode == INIT_DRIVE_CHANGED) {
            assert(rc == 0 && registers[0x1E] == 0x9400u);
        } else {
            assert(rc != 0);
            assert(strstr(output, "CONFIG_PASS_NOT_MEASUREMENT") == NULL);
        }
        if (mode == WAKE_NACK || mode == WAKE_RESET_NACK || mode == WAKE_COMMIT_NACK) {
            assert(rc == -2 && writes[11].index == 2u);
            assert(writes[11].bus.tx == 0x01u && writes[11].bus.ack_level == 1u);
            assert(strstr(output, "$B,RESULT,WAKE_WRITE,-2\r\n") != NULL);
        }
        if (mode == WAKE_RESET_NACK) {
            assert(strstr(output, "$Q,FIRST_READS,1A,0,2801\r\n") != NULL);
            assert(strstr(output, "$Q,FIRST_READS,1B,0,020F\r\n") != NULL);
            assert(strstr(output, "$Q,FIRST_READS,08,0,0080\r\n") != NULL);
            assert(strstr(output, "$K,PLUS20MS,HOLD,-18\r\n") != NULL);
        }
        if (mode == CORRUPT_RCOUNT) {
            assert(rc == -18 && strstr(output, "$B,RESULT,HOLD,-18\r\n") != NULL);
        }
        if (mode == WAKE_READ_NACK) {
            assert(rc == -2 && strstr(output, "$B,RESULT,WAKE_READ,-2\r\n") != NULL);
        }
        if (mode == DELAYED_RESET) {
            assert(rc == -18 && strstr(output, "$K,FIRST_READS,HOLD,0\r\n") != NULL);
            assert(strstr(output, "$K,PLUS20MS,HOLD,-18\r\n") != NULL);
        }
        if (mode == WRONG_ID || mode == NO_TARGET || mode == SETUP_NACK || mode == SLEEP_READ_NACK)
            assert(wake_seen == 0u);
        if (mode == DRIVE_FIELD_CORRUPT) {
            assert(rc == -18 && strstr(output, "$Q,FIRST_READS,1E,0,7800\r\n") != NULL);
        }
        if (mode == SLEEP_WRITE_NACK || mode == SLEEP_COMMIT_NACK) {
            assert(rc == -2 && wake_seen == 0u);
            assert(strstr(output, "$B,RESULT,SLEEP_WRITE,-2\r\n") != NULL);
        }
        if (mode == SLEEP_RCOUNT_LOSS || mode == SLEEP_NOW_NACK || mode == SLEEP_DATA_NACK) {
            assert(wake_seen == 0u && strstr(output, "$B,RESULT,SLEEP_NOW,") != NULL);
        }
        if (mode == SLEEP_DATA_NACK) {
            assert(rc == -2 && strstr(output, "$K,SLEEP_NOW,HOLD,0\r\n") != NULL);
            assert(strstr(output, "$K,SLEEP_NOW,COMM,-2\r\n") != NULL);
        }
        if (mode == SLEEP_RESET_ON_WAIT || mode == SLEEP_POSTWAIT_NACK) {
            assert(wake_seen == 0u && strstr(output, "$B,RESULT,SLEEP_1S,") != NULL);
            assert(strstr(output, "$K,SLEEP_NOW,HOLD,0\r\n") != NULL);
        }
        if (mode == UNEXPECTED_RESET_CONFIG) {
            assert(rc == -18 && sleep_seen == 0u && wake_seen == 0u);
            assert(strstr(output, "$B,RESULT,RESET_CONFIG,-18\r\n") != NULL);
        }
        if (!wake_seen) assert(strstr(output, "$K,WAKE,SKIPPED,-4\r\n") != NULL);
        if (!sleep_seen) assert(!g_wake_debug.sleep_now.captured);
        if (!wake_seen) assert(!g_wake_debug.plus20ms.captured);
        assert(g_wake_debug.done == 1u && g_wake_debug.result == rc);
        assert(quiet_wait_seen == (wake_seen ? 1u : 0u));
        if (wake_seen) {
            assert(g_wake_debug.wake_write_rc == writes[11].rc);
            assert(g_wake_debug.wake_payload_index == writes[11].index);
            assert(g_wake_debug.wake_bus.rc == writes[11].bus.rc);
        }
        if (mode == WAKE_COMMIT_NACK) {
            assert(registers[0x1A] == 0x1401u);
            assert(strstr(output, "$K,PLUS20MS,HOLD,0\r\n") != NULL);
        }
        if (mode == WAKE_STOP_FAIL) {
            assert(rc == -1 && writes[11].bus.stage == TEST_STAGE_STOP);
            assert(writes[11].bus.stop_rc == -1);
        }
        assert(explicit_resets <= 1u && wake_seen <= 1u);
        check_read_only_monitor();
    }
    /* Keil's one variable changes only the optional post-write quiet wait.
     * The artificial early-read fault demonstrates observability, not an
     * assertion that this is the real board's physical failure mechanism. */
    reset_model(OK);
    g_wake_quiet = 0u;
    assert(wake_diag_start() == 0 && quiet_wait_seen == 0u);
    assert(delay_count == 3u && delays[0] == 5u && delays[1] == 1000u && delays[2] == 20u);
    assert(first_wake_read_tick - wake_stop_tick < 5000u);
    assert(strstr(output, "$B,WAKE_QUIET_MS,0\r\n") != NULL);
    check_read_only_monitor();
    reset_model(EARLY_READ_RESET);
    g_wake_quiet = 0u;
    assert(wake_diag_start() == -18 && g_wake_debug.wake_write_rc == 0);
    assert(g_wake_debug.after_config == 0x2801u);
    check_read_only_monitor();
    reset_model(EARLY_READ_RESET);
    assert(wake_diag_start() == 0 && quiet_wait_seen == 1u);
    check_read_only_monitor();
    reset_model(QUIET_RESET);
    assert(wake_diag_start() == -18 && quiet_wait_seen == 1u);
    assert(g_wake_debug.after_config == 0x2801u);
    assert(g_wake_debug.first_reads.hold == -18);
    check_read_only_monitor();
    reset_model(WAKE_RESET_NACK);
    g_wake_quiet = 0u;
    assert(wake_diag_start() == -2 && g_wake_debug.wake_write_rc == -2);
    assert(g_wake_debug.after_config == 0x2801u);
    check_read_only_monitor();
    /* The real V3 capture failed already at the wake MSB. A failed high
     * byte must abort the frame: never transmit/commit the low byte, and
     * never reinterpret a later successful read as a successful write. */
    for (mode = WAKE_MSB_NACK; mode <= WAKE_MSB_RESET; ++mode) {
        reset_model(mode);
        assert(wake_diag_start() == -2);
        assert(wake_msb_attempted == 1u && wake_seen == 0u && written_count == 11u);
        assert(write_count == 12u && explicit_resets == 1u && quiet_wait_seen == 1u);
        assert(writes[11].index == 1u && writes[11].bus.tx == 0x14u);
        assert(writes[11].bus.ack_level == 1u && writes[11].bus.stop_rc == 0);
        assert(g_wake_debug.wake_write_rc == -2 && g_wake_debug.wake_read_rc == 0);
        assert(g_wake_debug.after_config == (mode == WAKE_MSB_NACK ? 0x3401u : 0x2801u));
        assert(g_wake_debug.plus20ms.hold == -18);
        assert(strstr(output, "$B,RESULT,WAKE_WRITE,-2\r\n") != NULL);
        check_read_only_monitor();
    }
    /* Every stable setting, not only CONFIG/RCOUNT, gates the wake write. */
    for (mode = SLEEP_FIELD_LOSS; mode <= SLEEP_LATE_FIELD_LOSS; ++mode) {
        static const unsigned stable_regs[] = {0x1A,0x1B,0x08,0x09,0x10,0x11,0x19,0x14,0x15,0x1E,0x1F};
        for (i = 0u; i < sizeof(stable_regs) / sizeof(stable_regs[0]); ++i) {
            reset_model(mode);
            lost_register = stable_regs[i];
            assert(wake_diag_start() == -18 && wake_seen == 0u && sleep_seen == 1u);
            assert(strstr(output, mode == SLEEP_FIELD_LOSS ?
                "$B,RESULT,SLEEP_NOW,-18\r\n" : "$B,RESULT,SLEEP_1S,-18\r\n") != NULL);
            check_read_only_monitor();
        }
    }
    puts("PASS: original test primitives, 54/register/MSB/LSB, each NACK index, STOP, timeout, timer wrap.");
    puts("PASS: original channel values/order; 3401 asleep, verified 1s hold, then only bit13 clears to 1401.");
    puts("PASS: relative to V4 only LDC reference bit9 changes; drive/activation settings remain unchanged.");
    puts("PASS: sleep write/read/config/1s-retention failures never issue the wake write; NACK is never forgiven.");
    puts("PASS: all 11 stable settings corrupted immediately or after 1s prevent waking (22 fault cases).");
    puts("PASS: wake/reset/read/STOP failures, delayed loss, fixed IDRIVE/read-only INIT_IDRIVE and no-data distinction.");
    puts("PASS: mixed COMM=-2/HOLD=-18 stay visible; no IRQ services, reference fallback or extra reset.");
    puts("PASS: five monitor rounds and a repeated start request never write register data or reset the LDC.");
    puts("PASS: Keil snapshots survive monitoring, safe checkpoints, 5ms quiet/0ms control, no NACK masking.");
    puts("PASS: early-read-only model fault distinguishes wait settings; resets during quiet still fail.");
    puts("PASS: wake-MSB NACK aborts before LSB, preserving either asleep or reset-like readback evidence.");
    puts("Digital model tests do not prove electrical behavior or real-board success.");
    return 0;
}
