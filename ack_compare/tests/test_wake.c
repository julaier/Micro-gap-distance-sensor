/* An edge-decoding slave, with optional input-sampler fault injection.
 * Execute unchanged A primitives or the independently implemented B driver.
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
static unsigned direction_changes, port_samples;
static int ack_sda_pattern, ack_scl_pattern;
#if ACK_COMPARE_PROFILE == 2
static int edge_sda_pattern, edge_scl_pattern;
#endif
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
            assert(value == 0x2A01u);
            ++sleep_seen;
            if (fault == SLEEP_WRITE_NACK || fault == SLEEP_COMMIT_NACK)
                ack_wanted = 0u;
            if (fault != SLEEP_WRITE_NACK) registers[pointer] = (uint16_t)value;
            if (fault == SLEEP_RCOUNT_LOSS) registers[0x09] = 0x0080u;
            if (fault == SLEEP_FIELD_LOSS)
                registers[lost_register] ^= lost_register >= 0x1Eu ? 0x0800u : 1u;
            return;
        }
        /* Original F103 activation (bit11=1), only bit13 changes from 2A01,
         * and never wake until both sleeping snapshots have passed. */
        assert(value == 0x0A01u && sleep_seen == 1u && held_wait_done);
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
                if (payload_count == 1u && pointer == 0x1Au && byte == 0x0Au) {
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
        #if ACK_COMPARE_PROFILE == 0
        assert(sda_input_mode);
#else
        assert(!sda_input_mode && sda_release); /* Released OD, no contention. */
#endif
        assert(level == (ack_wanted ? 0u : 1u));
        port_samples = 0u;
        ack_seen = 1u;
        break;
    case TX_BITS:
#if ACK_COMPARE_PROFILE == 0
        assert(sda_input_mode);
#else
        assert(!sda_input_mode && sda_release);
#endif
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
    ++direction_changes;
    unsigned before = bus();
    sda_release = 1;
    sda_input_mode = input;
    changed(before);
}
int test_scl_read(void) { return (bus() & 1u) != 0u; }
int test_sda_read(void) { return (bus() & 2u) != 0u; }
uint8_t test_bus_lines(void) { return (uint8_t)bus(); }
uint8_t ack_port_sample(void)
{
    unsigned pins = bus();
    if (active && phase == RX_ACK && ack_seen && scl_release && !force_scl_low) {
        assert(port_samples < 3u && !sda_input_mode && sda_release);
        if (ack_sda_pattern >= 0)
            pins = (pins & ~2u) | (((unsigned)ack_sda_pattern >> port_samples & 1u) << 1);
        if (ack_scl_pattern >= 0)
            pins = (pins & ~1u) | ((unsigned)ack_scl_pattern >> port_samples & 1u);
        ++port_samples;
    }
    return (uint8_t)pins;
}
#if ACK_COMPARE_PROFILE == 2
void ack_port_capture_edge(ack_edge_t *out)
{
    unsigned i, pins = bus();
    assert(!(pins & 1u) && !sda_input_mode && sda_release);
    out->before = (uint16_t)(((pins & 1u) << 7) | ((pins & 2u) << 5));
    test_scl_write(1);
    for (i = 0u; i < 8u; ++i) {
        pins = bus();
        if (edge_sda_pattern >= 0)
            pins = (pins & ~2u) | (((unsigned)edge_sda_pattern >> i & 1u) << 1);
        if (edge_scl_pattern >= 0)
            pins = (pins & ~1u) | ((unsigned)edge_scl_pattern >> i & 1u);
        out->raw[i] = (uint16_t)(((pins & 1u) << 7) | ((pins & 2u) << 5));
    }
    out->captured = 1u;
}
#endif
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
    direction_changes = port_samples = 0u;
    ack_sda_pattern = ack_scl_pattern = -1;
#if ACK_COMPARE_PROFILE == 2
    edge_sda_pattern = edge_scl_pattern = -1;
#endif
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
    target_config = 0x2A01u;
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

static void test_ack_samples(void)
{
    ack_trace_t evidence;
    test_i2c_diag_t d;
    unsigned i;
    reset_model(OK);
    assert(wake_i2c_write_reg(0x2Au, 0x08u, 0x1234u, 0) == 0);
    ack_i2c_get_trace(&evidence);
#if ACK_COMPARE_PROFILE == 0
    assert(evidence.count == 1u);
    assert(evidence.bytes[0].slot == 3u && evidence.bytes[0].tx == 0x34u);
    assert(evidence.bytes[0].samples == 1u && evidence.bytes[0].scl_mask == 255u);
    assert(evidence.bytes[0].dt_us[0] == ACK_TIME_UNKNOWN);
    assert(direction_changes != 0u);
    (void)i; (void)d;
#else
    assert(direction_changes == 0u && evidence.count == 4u);
    for (i = 0u; i < 4u; ++i) {
        static const unsigned tx[] = {0x54u, 0x08u, 0x12u, 0x34u};
        const ack_sample_t *s = &evidence.bytes[i];
        assert(s->slot == i && s->tx == tx[i] && s->clocks == 9u);
        assert(s->samples == 3u && s->sda_mask == 0u && s->scl_mask == 7u && s->rc == 0);
        assert(s->dt_us[0] >= 1u && s->dt_us[1] > s->dt_us[0]);
        assert(s->dt_us[2] > s->dt_us[1] && s->high_us >= s->dt_us[2]);
    }
    /* Inject changes only into the input sampler, NOT slave frame decoding:
     * all mixed patterns, including a late-low ACK, must fail explicitly. */
    for (i = 0u; i < 8u; ++i) {
        int expected = i == 0u ? 0 : i == 7u ? -2 : ACK_I2C_UNSTABLE;
        reset_model(OK);
        ack_sda_pattern = (int)i;
        assert(test_i2c_probe(0x2Au) == expected);
        ack_i2c_get_trace(&evidence);
        test_i2c_get_diag(&d);
        assert(evidence.count == 1u && evidence.bytes[0].sda_mask == i);
        assert(evidence.bytes[0].rc == expected && evidence.bytes[0].clocks == 9u);
        assert(starts == 1u && stops == 1u && bus() == 3u);
        assert(d.rc == expected && d.stop_rc == 0);
        if (i != 0u && i != 7u) assert(d.ack_level == 255u);
    }
    for (i = 0u; i < 7u; ++i) {
        reset_model(OK);
        ack_scl_pattern = (int)i;
        assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS);
        ack_i2c_get_trace(&evidence);
        assert(evidence.bytes[0].scl_mask == i && stops == 1u);
    }
    reset_model(OK);
    assert(test_i2c_init() == 0 && direction_changes == 1u);
    assert(test_i2c_probe(0x2Au) == 0 && direction_changes == 1u);
    puts("PASS: B three-point ACK patterns, no majority vote, SCL drop detection, OD direction stability.");
#endif
}

int main(void)
{
    static const unsigned expected_regs[] = {0x1C,0x14,0x15,0x10,0x11,0x08,0x09,0x1E,0x1F,0x1B,0x1A,0x1A};
    static const unsigned expected_values[] = {0x8000,0x1001,0x1001,0x0180,0x0180,0xFFFF,0xFFFF,0x9000,0x9000,0x820C,0x2A01,0x0A01};
    unsigned i, mode;
    int rc;
    test_adapter();
    /* Reproduce the actual original F103 read/modify/write arithmetic. */
    assert(CONFIG_F103_AWAKE == ((0x2801u & ~(0x2000u | 0x0200u)) | 0x0200u));
    assert(CONFIG_F103_AWAKE == 0x0A01u && CONFIG_F103_SLEEP == 0x2A01u);
    assert((CONFIG_F103_SLEEP ^ CONFIG_F103_AWAKE) == 0x2000u);
    assert((CONFIG_F103_AWAKE & 0x1400u) == 0u); /* Auto calibration/amplitude. */
    test_ack_samples();
    reset_model(OK);
    assert(wake_diag_start() == 0);
    assert(written_count == 12u && explicit_resets == 1u && sleep_seen == 1u && wake_seen == 1u);
    for (i = 0u; i < 12u; ++i) {
        assert(written_regs[i] == expected_regs[i] && written_values[i] == expected_values[i]);
    }
    assert(delay_count == 4u && delays[0] == 5u && delays[1] == 1000u &&
           delays[2] == 5u && delays[3] == 20u);
    assert(quiet_wait_seen == 1u && first_wake_read_tick - wake_stop_tick >= 5000u);
    assert(g_wake_debug.magic == 0x41434B36u && g_wake_debug.mcu_reset_flags == 0x04000000u);
    assert(g_wake_debug.done == 1u && g_wake_debug.phase == WAKE_DEBUG_DONE);
    assert(g_wake_debug.checkpoint == 2u && g_wake_debug.result == 0);
    assert(g_wake_debug.before_config == 0x2801u && g_wake_debug.after_config == 0x0A01u);
    assert(g_wake_debug.wake_write_rc == 0 && g_wake_debug.wake_read_rc == 0);
#if ACK_COMPARE_PROFILE != 0
    assert(g_wake_debug.sleep_ack.count == 4u && g_wake_debug.wake_ack.count == 4u);
    assert(g_wake_debug.sleep_ack.bytes[2].tx == 0x2Au);
    assert(g_wake_debug.wake_ack.bytes[2].tx == 0x0Au);
    assert(g_wake_debug.wake_ack.bytes[3].tx == 0x01u);
    assert(strstr(output, "$A,WAKE,3,01,3,0,7,") != NULL);
#else
    assert(g_wake_debug.sleep_ack.count == 1u && g_wake_debug.wake_ack.count == 1u);
    assert(strstr(output, "$A,WAKE,3,01,1,0,255,65535,") != NULL);
#endif
    assert(g_wake_debug.sleep_now.hold == 0 && g_wake_debug.sleep_1s.hold == 0);
    assert(g_wake_debug.sleep_now.captured && g_wake_debug.sleep_1s.captured);
    assert(g_wake_debug.first_reads.captured && g_wake_debug.plus20ms.captured);
    assert(g_wake_debug.first_reads.comm == 0 && g_wake_debug.plus20ms.hold == 0);
    assert(strstr(output, "$B,WAKE_QUIET_MS,5\r\n") != NULL);
    assert(written_at_sleep_wait == 11u);
    assert(output_at_first_config != 0u && output_at_first_config == output_at_wake);
    assert(strstr(output, "$S,1A,2A01,0,0,2A01\r\n") != NULL);
    assert(strstr(output, "$Q,SLEEP_NOW,1A,0,2A01\r\n") != NULL);
    assert(strstr(output, "$Q,SLEEP_1S,1A,0,2A01\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_NOW,COMM,0\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_NOW,HOLD,0\r\n") != NULL);
    assert(strstr(output, "$K,SLEEP_1S,HOLD,0\r\n") != NULL);
    assert(strstr(output, "$C,1A,0A01,0,0,0A01\r\n") != NULL);
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
        if (mode == INIT_DRIVE_CHANGED || mode == DRIVE_FIELD_CORRUPT) {
            assert(rc == 0 && registers[0x1E] == (mode == INIT_DRIVE_CHANGED ? 0x9400u : 0x7800u));
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
            assert(rc == 0 && strstr(output, "$Q,FIRST_READS,1E,0,7800\r\n") != NULL);
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
            assert(registers[0x1A] == 0x0A01u);
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
        assert(writes[11].index == 1u && writes[11].bus.tx == 0x0Au);
        assert(writes[11].bus.ack_level == 1u && writes[11].bus.stop_rc == 0);
        assert(g_wake_debug.wake_write_rc == -2 && g_wake_debug.wake_read_rc == 0);
        assert(g_wake_debug.after_config == (mode == WAKE_MSB_NACK ? 0x2A01u : 0x2801u));
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
    puts("PASS: " ACK_PROFILE_NAME " register frames, ACK/NACK indices, STOP, timeout, timer wrap.");
    puts("PASS: original F103 CONFIG 2A01/0A01, channel settings/order and 1-second sleep retention.");
    puts("PASS: 22 sleep-field-loss cases gate wake; active auto-IDRIVE changes are not false failures.");
    puts("PASS: NACK never forgiven by readback; mixed COMM/HOLD faults and default-register loss retained.");
    puts("PASS: one reset/one wake; safe Keil checkpoints and persistent traces; monitor is read-only.");
    puts("Digital model tests do not prove electrical timing or real-board success.");
#if ACK_COMPARE_PROFILE == 2
    {
        ack_trace_t evidence;
        wake_first_failure_t saved;
        /* Early low transitioning high is evidence, NOT permission to
         * ignore a later NACK. Raw SCL-low is not a valid ACK sample. */
        reset_model(OK);
        ack_sda_pattern = 7;
        edge_sda_pattern = 0xF0;
        edge_scl_pattern = 0xFE;
        assert(test_i2c_probe(0x2Au) == TEST_I2C_NACK);
        ack_i2c_get_trace(&evidence);
        assert(evidence.count == 1u && evidence.bytes[0].edge.captured);
        assert((evidence.bytes[0].edge.before & 128u) == 0u);
        for (i = 0u; i < 8u; ++i) {
            unsigned expected = (i == 0u ? 0u : 128u) | (i < 4u ? 0u : 64u);
            assert(evidence.bytes[0].edge.raw[i] == expected);
        }
        assert(evidence.bytes[0].sda_mask == 7u && evidence.bytes[0].rc == -2);

        reset_model(WAKE_RESET_NACK);
        assert(wake_diag_start() == -2);
        assert(g_wake_debug.first_failure.captured);
        assert(g_wake_debug.first_failure.operation == 'W');
        assert(g_wake_debug.first_failure.reg == 0x1Au);
        assert(g_wake_debug.first_failure.payload_index == 2u);
        assert(g_wake_debug.first_failure.ack.bytes[3].tx == 0x01u);
        assert(strstr(output, "$J,WAKE,3,01,") != NULL);
        assert(strstr(output, "$F,FIRST,W,1A,0A01,") != NULL);
        saved = g_wake_debug.first_failure;
        check_read_only_monitor();
        assert(memcmp((const void *)&g_wake_debug.first_failure, &saved, sizeof(saved)) == 0);

        reset_model(SLEEP_NOW_NACK);
        assert(wake_diag_start() == -2);
        assert(g_wake_debug.first_failure.operation == 'R');
        assert(g_wake_debug.first_failure.reg == 0x11u);
        assert(g_wake_debug.first_failure.phase == WAKE_DEBUG_SLEEP_NOW);
        assert(g_wake_debug.first_failure.bus.stage == TEST_STAGE_ADDR_R);
        assert(g_wake_debug.first_failure.ack.bytes[2].tx == 0x55u);
        assert(strstr(output, "$F,FIRST,R,11,0000,") != NULL);
        saved = g_wake_debug.first_failure;
        check_read_only_monitor();
        assert(memcmp((const void *)&g_wake_debug.first_failure, &saved, sizeof(saved)) == 0);

        reset_model(NO_TARGET);
        assert(wake_diag_start() == -2);
        assert(g_wake_debug.first_failure.operation == 'W');
        assert(g_wake_debug.first_failure.reg == 0x1Cu);
        assert(g_wake_debug.first_failure.phase == WAKE_DEBUG_RESET);
        assert(g_wake_debug.first_failure.ack.count == 1u);
        assert(g_wake_debug.first_failure.ack.bytes[0].tx == 0x54u);
        check_read_only_monitor();

        reset_model(OK);
        assert(wake_diag_start() == 0 && !g_wake_debug.first_failure.captured);
        monitor_fault = 1u;
        wake_diag_monitor(1u);
        assert(!g_wake_debug.first_failure.captured);
        assert(strstr(output, "$F,FIRST,NONE\r\n") != NULL);
        puts("PASS: C early IDR evidence never overrides NACK; first startup error survives snapshots/monitor.");
    }
#endif
    return 0;
}
