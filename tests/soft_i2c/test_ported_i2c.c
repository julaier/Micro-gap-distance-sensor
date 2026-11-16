/* V12 original test core + production adapter + an edge-decoding slave.
 * Adapter read traces are compared with direct calls to that same source.
 * No scripted SDA samples and no production protocol functions are mocked.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hard_i2c.h"
#include "soft_i2c_port.h"
#include "ldc1614.h"
#include "test_board.h"
#include "test_i2c.h"

enum { RX_BITS, RX_ACK, TX_BITS, TX_ACK, WAIT_STOP };
static unsigned phase, bits, byte, address_byte, selected, reading, ack_wanted;
static unsigned active, ack_seen, read_index, pointer, payload_count, high_byte;
static unsigned starts, stops, master_acks[1024], master_ack_count;
static unsigned address_count, addresses[2048], slave_address;
static unsigned trace[65536], trace_count;
static uint16_t registers[128];
static int scl_release, sda_release, sda_input_mode, slave_low;
static int force_scl_low, force_sda_low, nack_data_index, nack_read_address;
static unsigned raise_attempts, fail_raise, recovery_release_at, recovery_clocks;
static int hold_sda_on_stop;
static uint32_t ticks;
/* Synthetic CONFIG faults exercise handling; they are not a hardware diagnosis.
 * 1=reject final byte, 2=commit but NACK, 3=ACK wrong readback,
 * 4=read-address NACK after commit, 5=reject high byte, 6=failed STOP. */
static unsigned config_fault, fault_config_number, config_writes;
static unsigned register_writes;
static uint16_t config_values[8];
/* Clock-aware digital conversion model, not an analog/oscillator simulation. */
static unsigned conversions_enabled, external_ref_running, internal_ref_running;
static unsigned conversion_count, conversion_limit;
static uint32_t next_conversion_ticks, conversion_value0, conversion_value1;

static unsigned bus(void)
{
    return (scl_release && !force_scl_low ? 1u : 0u) |
           ((sda_release || sda_input_mode) && !slave_low && !force_sda_low ? 2u : 0u);
}

static void reset_registers(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0x7E] = 0x5449u;
    registers[0x7F] = 0x3055u;
    registers[0x1A] = 0x2801u;
    registers[0x1B] = 0x020Fu;
    registers[0x08] = registers[0x09] = 0x0080u;
    conversion_count = 0u;
}

static unsigned tx_value(void)
{
    if (pointer == 0x18u && read_index == 0u && conversions_enabled &&
        !(registers[0x1A] & 0x2000u) &&
        ((registers[0x1A] & 0x0200u) ? external_ref_running : internal_ref_running) &&
        ticks >= next_conversion_ticks && conversion_count < conversion_limit) {
        registers[0] = (uint16_t)(conversion_value0 >> 16);
        registers[1] = (uint16_t)conversion_value0;
        registers[2] = (uint16_t)(conversion_value1 >> 16);
        registers[3] = (uint16_t)conversion_value1;
        registers[0x18] |= 0x004Cu;
        ++conversion_count;
        next_conversion_ticks = ticks + 60000u;
    }
    uint16_t value = registers[pointer & 127u];
    return (read_index & 1u) == 0u ? value >> 8 : value & 255u;
}

static void set_tx_bit(void)
{
    slave_low = (tx_value() & (0x80u >> bits)) == 0u;
}

static void rising(void)
{
    unsigned level = (bus() & 2u) != 0u;
    if (!active) return;
    switch (phase) {
    case RX_BITS:
        byte = (byte << 1) | level;
        if (++bits == 8u) {
            if (address_byte) {
                assert(address_count < sizeof(addresses) / sizeof(addresses[0]));
                addresses[address_count++] = byte;
                reading = byte & 1u;
                selected = (byte >> 1) == slave_address;
                ack_wanted = selected && !(reading && nack_read_address) &&
                    !(reading && pointer == 0x18u && config_fault == 12u);
            } else {
                assert(selected);
                ack_wanted = (int)payload_count != nack_data_index;
                if (pointer == 0x1Au && payload_count == 1u &&
                    config_fault == 5u && config_writes + 1u == fault_config_number)
                    ack_wanted = 0u;
                if (ack_wanted) {
                    if (payload_count == 0u) pointer = byte;
                    else if (payload_count == 1u) high_byte = byte;
                    else if (payload_count == 2u) {
                        unsigned value = (high_byte << 8) | byte;
                        int suppress = 0;
                        ++register_writes;
                        assert(pointer < 128u && pointer != 0x7Eu && pointer != 0x7Fu);
                        if (pointer == 0x1Au) {
                            assert(config_writes < 8u);
                            config_values[config_writes++] = (uint16_t)value;
                            if (config_writes == fault_config_number) {
                                if (config_fault == 1u || config_fault == 2u) ack_wanted = 0u;
                                if (config_fault == 3u) value ^= 1u;
                                if (config_fault == 4u) nack_read_address = 1;
                                if (config_fault == 6u) hold_sda_on_stop = 1;
                            }
                            /* 7=external wake reset; 8=reject both wake writes;
                             * 9=CONFIG-only reset; 10=reset on BOTH wake writes;
                             * 11=ACK wake, but lose RCOUNT; 12=STATUS read NACK.
                             * All are synthetic fault injections, not causes. */
                            if (config_fault == 8u && !(value & 0x2000u)) {
                                ack_wanted = 0u;
                                suppress = 1;
                            }
                            if (((config_fault == 7u || config_fault == 9u) &&
                                 (value & 0x2200u) == 0x0200u) ||
                                (config_fault == 10u && !(value & 0x2000u))) {
                                ack_wanted = 0u;
                                if (config_fault != 9u) reset_registers();
                                else registers[0x1A] = 0x2801u;
                                suppress = 1;
                            }
                            if (config_fault == 11u && !(value & 0x2000u))
                                registers[0x08] = 0x0080u;
                            if (!(value & 0x2000u)) next_conversion_ticks = ticks + 60000u;
                        }
                        if (pointer == 0x1Cu && value == 0x8000u) reset_registers();
                        else if (!suppress && !(pointer == 0x1Au && config_writes == fault_config_number &&
                                   config_fault == 1u)) registers[pointer] = (uint16_t)value;
                    }
                }
                ++payload_count;
            }
            phase = RX_ACK;
            ack_seen = 0u;
        }
        break;
    case RX_ACK:
        assert(sda_input_mode); /* The slave must own SDA for every ACK. */
        assert(level == (ack_wanted ? 0u : 1u));
        ack_seen = 1u;
        break;
    case TX_BITS:
        assert(sda_input_mode);
        if (++bits == 8u) { phase = TX_ACK; ack_seen = 0u; }
        break;
    case TX_ACK:
        assert(!sda_input_mode && master_ack_count < sizeof(master_acks) / sizeof(master_acks[0]));
        master_acks[master_ack_count++] = level;
        ack_seen = 1u;
        break;
    default: break;
    }
}

static void falling(void)
{
    if (!active) return;
    switch (phase) {
    case RX_ACK:
        if (!ack_seen) slave_low = (int)ack_wanted;
        else {
            slave_low = 0;
            bits = byte = 0u;
            if (!ack_wanted) phase = WAIT_STOP;
            else if (address_byte && reading) {
                phase = TX_BITS;
                read_index = 0u;
                set_tx_bit();
            } else phase = RX_BITS;
            address_byte = 0u;
        }
        break;
    case TX_BITS:
        set_tx_bit();
        break;
    case TX_ACK:
        if (!ack_seen) slave_low = 0;
        else if (master_acks[master_ack_count - 1u]) {
            if (pointer == 1u) registers[0x18] &= (uint16_t)~0x0008u;
            if (pointer == 3u) registers[0x18] &= (uint16_t)~0x0004u;
            phase = WAIT_STOP;
        }
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
    assert(trace_count < sizeof(trace) / sizeof(trace[0]));
    trace[trace_count++] = after | (sda_input_mode ? 4u : 0u);
    if ((before & 1u) && (after & 1u) && ((before ^ after) & 2u)) {
        if (after & 2u) { ++stops; active = 0u; slave_low = 0; }
        else {
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

void i2c_port_init(void)
{
    scl_release = sda_release = 1;
    sda_input_mode = 0;
}

void i2c_port_scl_write(int release)
{
    unsigned before = bus();
    if (release) {
        if (++raise_attempts == fail_raise) force_scl_low = 1;
        if (!scl_release && !active && force_sda_low && recovery_release_at) {
            if (++recovery_clocks == recovery_release_at) force_sda_low = 0;
        }
    }
    scl_release = release;
    changed(before);
}

void i2c_port_sda_write(int release)
{
    unsigned before = bus();
    assert(release || !sda_input_mode);
    if (release && scl_release && active && hold_sda_on_stop) force_sda_low = 1;
    sda_release = release;
    changed(before);
}

void i2c_port_sda_input(int input)
{
    unsigned before = bus();
    if (input != sda_input_mode && active) assert(!(before & 1u));
    sda_release = 1;
    sda_input_mode = input;
    changed(before);
}

int i2c_port_scl_read(void) { return (bus() & 1u) != 0u; }
int i2c_port_sda_read(void) { return (bus() & 2u) != 0u; }
uint8_t i2c_port_lines(void) { return (uint8_t)bus(); }
uint16_t i2c_port_time_us(void) { return (uint16_t)++ticks; }
void i2c_port_delay_us(uint16_t us) { assert(us >= 5u); ticks += us; }
uint8_t i2c_port_sda_mode(void) { return sda_input_mode ? 0u : 1u; }
uint8_t i2c_port_sda_od(void) { return 1u; }
uint8_t i2c_port_pull_test(void) { assert(bus() == 3u && !active); return 15u; }
void board_delay_ms(uint32_t ms) { ticks += ms * 1000u; }

/* Link the unchanged standalone core to precisely the same virtual wires. */
void test_scl_write(int release) { i2c_port_scl_write(release); }
void test_sda_write(int release) { i2c_port_sda_write(release); }
void test_sda_input(int input) { i2c_port_sda_input(input); }
int test_scl_read(void) { return i2c_port_scl_read(); }
int test_sda_read(void) { return i2c_port_sda_read(); }
uint8_t test_bus_lines(void) { return i2c_port_lines(); }
uint16_t test_time_us(void) { return i2c_port_time_us(); }
void test_delay_us(uint16_t us) { i2c_port_delay_us(us); }

static void reset_model(unsigned addr)
{
    phase = RX_BITS;
    bits = byte = address_byte = selected = reading = ack_wanted = 0u;
    active = ack_seen = read_index = pointer = payload_count = high_byte = 0u;
    starts = stops = master_ack_count = address_count = trace_count = 0u;
    memset(addresses, 0, sizeof(addresses));
    scl_release = sda_release = 1;
    sda_input_mode = slave_low = force_scl_low = force_sda_low = 0;
    nack_data_index = -1;
    nack_read_address = hold_sda_on_stop = 0;
    raise_attempts = fail_raise = recovery_release_at = recovery_clocks = 0u;
    ticks = 0u;
    config_fault = fault_config_number = config_writes = register_writes = 0u;
    conversions_enabled = 0u;
    external_ref_running = internal_ref_running = 1u;
    conversion_limit = 0xFFFFFFFFu;
    conversion_value0 = 120000u;
    conversion_value1 = 100000u;
    next_conversion_ticks = 0u;
    memset(config_values, 0, sizeof(config_values));
    slave_address = addr;
    reset_registers();
    hard_i2c_init();
    trace_count = raise_attempts = 0u;
}

static void assert_idle(void)
{
    assert(bus() == 3u && !active && !slave_low && !sda_input_mode);
}

static int read_reg(unsigned address, unsigned reg, uint16_t *value)
{
    uint8_t pointer_byte = (uint8_t)reg, rx[2];
    int rc = hard_i2c_write_read((uint8_t)address, &pointer_byte, 1u, rx, 2u);
    if (rc == 0) *value = (uint16_t)((rx[0] << 8) | rx[1]);
    return rc;
}

static void compare_standalone(unsigned address, unsigned reg, int nack)
{
    unsigned oracle_trace[1024], count;
    uint16_t expected = 0u, actual = 0u;
    hard_i2c_diag_t d;
    reset_model(address);
    nack_data_index = nack ? 0 : -1;
    assert(test_i2c_read_reg((uint8_t)address, (uint8_t)reg, &expected) == (nack ? -2 : 0));
    count = trace_count;
    assert(count < 1024u);
    memcpy(oracle_trace, trace, count * sizeof(trace[0]));
    reset_model(address);
    nack_data_index = nack ? 0 : -1;
    assert(read_reg(address, reg, &actual) == (nack ? -2 : 0));
    assert(actual == expected && trace_count == count);
    assert(memcmp(oracle_trace, trace, count * sizeof(trace[0])) == 0);
    hard_i2c_get_diag(&d);
    assert(d.stop_rc == 0 && d.ack_sda_mode == 255u && d.ack_sda_od == 255u);
    assert(d.ack_level == (nack ? 1u : 0u) && d.clocks == 9u);
    assert(d.half_ticks == 5u && d.effective_hz > 10000u && d.effective_hz <= 100000u);
    assert(d.sampled_byte == 255u && d.ack_samples == 255u);
    assert_idle();
}

static void test_full_ldc(void)
{
    uint16_t mid = 0u, did = 0u, flags = 0u, status = 0u;
    uint32_t ch0 = 0u, ch1 = 0u;
    reset_model(0x2Au);
    ldc1614_init_driver();
    assert(ldc1614_init(&did, &mid) == 0 && mid == 0x5449u && did == 0x3055u);
    assert(registers[0x14] == 0x1001u && registers[0x15] == 0x1001u);
    assert(registers[0x10] == 0x0180u && registers[0x11] == 0x0180u);
    assert(registers[0x08] == 0xFFFFu && registers[0x09] == 0xFFFFu);
    assert(registers[0x1E] == 0x9000u && registers[0x1F] == 0x9000u);
    assert(registers[0x1B] == 0x820Cu && registers[0x1A] == 0x0A01u);
    assert(registers[0x19] == 1u); /* DRDY_2INT is enabled, not just NVIC. */
    assert(config_writes == 2u && config_values[0] == 0x2A01u && config_values[1] == 0x0A01u);
    registers[0] = 0x0123u; registers[1] = 0x4567u;
    registers[2] = 0x0456u; registers[3] = 0x789Au;
    assert(ldc1614_read_raw(&ch0, &ch1, &flags) == 0);
    assert(ch0 == 0x01234567u && ch1 == 0x0456789Au && flags == 0u);
    registers[0] |= 0x8000u;
    registers[2] |= 0x1000u;
    assert(ldc1614_read_raw(&ch0, &ch1, &flags) == 0 && flags == 0x9000u);
    registers[0x18] = 0x0008u;
    assert(ldc1614_read_status(&status) == 0 && status == 0x0008u);
    assert(master_ack_count != 0u && master_ack_count % 2u == 0u);
    for (unsigned i = 0u; i < master_ack_count; ++i) assert(master_acks[i] == (i & 1u));
    assert_idle();
    puts("PASS: sleep/external-clock/wake order, all configuration readbacks, INTB enable and dual-channel data.");
}

static ldc1614_init_step_t init_steps[16];
static unsigned init_step_count;
static void capture_init_step(const ldc1614_init_step_t *step)
{
    assert(init_step_count < 16u);
    init_steps[init_step_count++] = *step;
}

static void test_internal_reference_and_freshness(void)
{
    uint32_t a = 0u, b = 0u;
    uint16_t err = 0u, st = 0u;
    reset_model(0x2Au);
    assert(ldc1614_init_clock(0, 0, 0) == 0);
    assert(config_values[0] == 0x2801u && config_values[1] == 0x0801u);
    assert(registers[0x14] == 0x1001u && registers[0x15] == 0x1001u);
    registers[0] = 1u; registers[1] = 2u;
    registers[2] = 3u; registers[3] = 4u;
    assert(ldc1614_read_fresh_raw(&a, &b, &err, &st) == 1 && a == 0u && b == 0u);
    registers[0x18] = 8u; /* CH0 alone must not yield a mismatched pair. */
    assert(ldc1614_read_fresh_raw(&a, &b, &err, &st) == 1 && st == 8u);
    registers[0x18] = 12u;
    assert(ldc1614_read_fresh_raw(&a, &b, &err, &st) == 0);
    assert(a == 0x10002u && b == 0x30004u && registers[0x18] == 0u);
    assert(ldc1614_read_fresh_raw(&a, &b, 0, 0) == 1); /* Same DATA, now stale. */
    registers[0] |= 0x8000u;
    registers[0x18] = 12u;
    assert(ldc1614_read_fresh_raw(&a, &b, &err, &st) == -20 && err == 0x8000u);
    registers[0] = registers[1] = 0u;
    registers[0x18] = 12u;
    assert(ldc1614_read_fresh_raw(&a, &b, &err, &st) == -20 && a == 0u);
    puts("PASS: internal reference changes only bit9; both unread flags, stale, error and zero data guards.");
}

static void test_config_faults(void)
{
    hard_i2c_diag_t d;
    for (unsigned number = 1u; number <= 2u; ++number) {
        for (unsigned fault = 1u; fault <= 6u; ++fault) {
            int rc;
            ldc1614_init_step_t *last;
            reset_model(0x2Au);
            config_fault = fault;
            fault_config_number = number;
            init_step_count = 0u;
            ldc1614_set_init_trace(capture_init_step);
            rc = ldc1614_init(0, 0);
            last = &init_steps[init_step_count - 1u];
            assert(rc == (fault == 2u ? 0 : -17));
            if (fault == 2u) {
                unsigned index = number == 1u ? init_step_count - 2u : init_step_count - 1u;
                assert(init_steps[index].write_rc == -2 && init_steps[index].nack_verified == 1u);
                assert(init_steps[index].read_rc == 0 && init_steps[index].actual == init_steps[index].expected);
                assert(registers[0x1A] == 0x0A01u);
                assert_idle();
            } else {
                assert(last->result != 0 && last->nack_verified == 0u);
                ldc1614_get_init_bus_diag(&d);
                if (fault == 1u) {
                    assert(last->write_rc == -2 && last->read_rc == 0 && last->actual != last->expected);
                    assert(d.rc == -2 && d.stage == HARD_I2C_WRITE && d.byte_index == 2u);
                } else if (fault == 3u) {
                    assert(last->write_rc == 0 && last->read_rc == 0 && last->actual != last->expected);
                } else if (fault == 4u) {
                    assert(last->write_rc == 0 && last->read_rc == -2 && d.stage == HARD_I2C_ADDR_R);
                } else if (fault == 5u) {
                    assert(last->write_rc == -2 && last->read_rc == -4 && d.byte_index == 1u);
                } else {
                    assert(last->write_rc == -1 && last->read_rc == -4 && d.stop_rc == -1);
                }
            }
        }
    }
    ldc1614_set_init_trace(0);
    puts("PASS: 12 sleep/wake CONFIG fault cases; NACK is accepted only after exact verified readback.");
}

static void test_failures(void)
{
    hard_i2c_diag_t d;
    uint8_t reg = 0x7Eu, rx[2] = {0xAAu, 0xBBu}, tx[] = {0x10u, 0x01u, 0x80u};
    uint32_t before;
    for (unsigned index = 0u; index < 3u; ++index) {
        reset_model(0x2Au);
        nack_data_index = (int)index;
        assert(hard_i2c_write(0x2Au, tx, 3u) == -2);
        hard_i2c_get_diag(&d);
        assert(d.rc == -2 && d.stage == HARD_I2C_WRITE && d.byte_index == index);
        assert(d.tx_byte == tx[index] && d.ack_level == 1u && d.stop_rc == 0);
        assert_idle();
    }
    reset_model(0x2Au);
    nack_read_address = 1;
    assert(hard_i2c_write_read(0x2Au, &reg, 1u, rx, 2u) == -2);
    hard_i2c_get_diag(&d);
    assert(d.stage == HARD_I2C_ADDR_R && d.tx_byte == 0x55u && d.stop_rc == 0);
    assert(rx[0] == 0xAAu && rx[1] == 0xBBu);
    assert_idle();

    reset_model(0x2Au);
    fail_raise = 10u; /* Address ACK clock cannot rise. */
    before = ticks;
    assert(hard_i2c_write(0x2Au, 0, 0u) == -1);
    hard_i2c_get_diag(&d);
    assert(d.stage == HARD_I2C_ADDR_W && d.clocks == 8u && d.ack_level == 255u);
    assert(d.stop_rc == -1 && !sda_input_mode && ticks - before < 5000u);

    reset_model(0x2Au);
    fail_raise = 30u; /* First received data bit cannot rise. */
    assert(hard_i2c_write_read(0x2Au, &reg, 1u, rx, 2u) == -1);
    hard_i2c_get_diag(&d);
    assert(d.stage == HARD_I2C_READ && d.byte_index == 0u && !sda_input_mode);
    assert(rx[0] == 0xAAu && rx[1] == 0xBBu);

    reset_model(0x2Au);
    force_scl_low = 1;
    before = ticks;
    assert(hard_i2c_write(0x2Au, 0, 0u) == -1 && ticks - before < 5000u);
    hard_i2c_get_diag(&d);
    assert(d.stage == HARD_I2C_START && d.clocks == 0u && d.ack_level == 255u);

    reset_model(0x2Au);
    force_sda_low = 1;
    assert(hard_i2c_write(0x2Au, 0, 0u) == -1);
    hard_i2c_get_diag(&d);
    assert(d.stage == HARD_I2C_START && d.stop_rc == -1);

    reset_model(0x2Au);
    hold_sda_on_stop = 1;
    assert(hard_i2c_write(0x2Bu, 0, 0u) == -2);
    hard_i2c_get_diag(&d);
    assert(d.rc == -2 && d.stage == HARD_I2C_ADDR_W && d.stop_rc == -1);
    reset_model(0x2Au);
    hold_sda_on_stop = 1;
    assert(hard_i2c_write(0x2Au, 0, 0u) == -1);
    hard_i2c_get_diag(&d);
    assert(d.rc == -1 && d.stage == HARD_I2C_STOP && d.stop_rc == -1);
    puts("PASS: data/read-address NACKs, bounded ACK/read/start timeouts and separate STOP errors.");
}

int main(void)
{
    hard_i2c_diag_t d;
    uint8_t reg = 0x7Eu, rx[2], levels[4];
    uint16_t value;
    unsigned count;
    compare_standalone(0x2Au, 0x7Eu, 0);
    compare_standalone(0x2Au, 0x7Fu, 0);
    compare_standalone(0x2Bu, 0x7Eu, 0);
    compare_standalone(0x2Au, 0x7Eu, 1);
    puts("PASS: production START/bit/ACK/INPUT/repeated-START/read/STOP order matches standalone test.");

    reset_model(0x2Au);
    for (unsigned address = 0x08u; address <= 0x77u; ++address) {
        int rc = hard_i2c_write((uint8_t)address, 0, 0u);
        assert(rc == (address == 0x2Au ? 0 : -2));
        assert(addresses[address_count - 1u] == address << 1);
        hard_i2c_get_diag(&d);
        assert(d.ack_level == (rc == 0 ? 0u : 1u) && d.clocks == 9u && d.stop_rc == 0);
        assert_idle();
    }
    assert(address_count == 112u);
    ticks = 65500u;
    assert(read_reg(0x2Au, 0x7Eu, &value) == 0 && value == 0x5449u);
    assert(hard_i2c_write_read(0x2Au, &reg, 1u, rx, 1u) == 0 && rx[0] == 0x54u);
    assert(master_acks[master_ack_count - 1u] == 1u);
    hard_i2c_pin_test(levels);
    assert(levels[0] == 3u && levels[1] == 2u && levels[2] == 1u && levels[3] == 3u);
    assert(hard_i2c_lines() == 255u);
    count = starts;
    assert(hard_i2c_write(0x80u, 0, 0u) == -3);
    assert(hard_i2c_write(0x2Au, 0, 1u) == -3);
    assert(hard_i2c_write_read(0x2Au, &reg, 1u, rx, 3u) == -3);
    assert(hard_i2c_write_read(0x2Au, 0, 1u, rx, 2u) == -3);
    assert(hard_i2c_write_read(0x2Au, &reg, 1u, 0, 2u) == -3);
    assert(hard_i2c_calibrate_100k(0x2Au) == -3 && starts == count);
    hard_i2c_debug_config(1u, 0u); /* Legacy flags cannot change the working mapping/mode. */
    assert(read_reg(0x2Au, 0x7Eu, &value) == 0 && value == 0x5449u);
    assert_idle();
    puts("PASS: 112 shifted addresses, timer wrap, single-byte final NACK, GPIO checks and invalid arguments.");

    reset_model(0x2Au);
    force_sda_low = 1;
    recovery_release_at = 4u;
    hard_i2c_init();
    hard_i2c_get_diag(&d);
    assert(d.rc == 0 && recovery_clocks == 4u);
    assert_idle();
    test_full_ldc();
    test_internal_reference_and_freshness();
    test_config_faults();
    test_failures();
    puts("All V12 production I2C tests passed. Digital tests do not establish real-board success.");
    return 0;
}
