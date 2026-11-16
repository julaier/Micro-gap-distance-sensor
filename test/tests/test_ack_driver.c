/* Independent edge-decoding slave model for the NEW test driver.
 * The slave determines ACK from bytes actually clocked on the virtual bus.
 * No MCU register mocks or production hard_i2c.c are used here.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_board.h"
#include "test_i2c.h"

enum { RX_BITS, RX_ACK, TX_BITS, TX_ACK, WAIT_STOP };
static unsigned phase, bits, byte, address_byte, selected, reading, ack_wanted;
static unsigned active, ack_seen, read_index, pointer, payload_count;
static unsigned starts, stops, master_acks[8], master_ack_count;
static unsigned address_count, addresses[256], slave_address;
static int scl_release, sda_release, sda_input_mode, slave_low;
static int force_scl_low, force_sda_low, nack_pointer, nack_read_address;
static uint32_t ticks;

static unsigned bus(void)
{
    return (scl_release && !force_scl_low ? 1u : 0u) |
           ((sda_release || sda_input_mode) && !slave_low && !force_sda_low ? 2u : 0u);
}

static unsigned tx_value(void)
{
    unsigned reg = pointer == 0x7Eu ? 0x5449u : pointer == 0x7Fu ? 0x3055u : 0u;
    return read_index == 0u ? reg >> 8 : reg & 255u;
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
                assert(address_count < 256u);
                addresses[address_count++] = byte;
                reading = byte & 1u;
                selected = (byte >> 1) == slave_address;
                ack_wanted = selected && !(reading && nack_read_address);
            } else {
                /* This test driver must only send ONE register-pointer byte. */
                assert(selected && payload_count == 0u);
                pointer = byte;
                payload_count++;
                ack_wanted = !nack_pointer;
            }
            phase = RX_ACK;
            ack_seen = 0u;
        }
        break;
    case RX_ACK:
        assert(sda_input_mode); /* Explicit input is required for slave ACK. */
        assert(level == (ack_wanted ? 0u : 1u));
        ack_seen = 1u;
        break;
    case TX_BITS:
        assert(sda_input_mode);
        if (++bits == 8u) { phase = TX_ACK; ack_seen = 0u; }
        break;
    case TX_ACK:
        assert(!sda_input_mode && master_ack_count < 8u);
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
        else if (master_acks[master_ack_count - 1u]) phase = WAIT_STOP;
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

void test_scl_write(int release)
{
    unsigned before = bus();
    scl_release = release;
    changed(before);
}

void test_sda_write(int release)
{
    unsigned before = bus();
    sda_release = release;
    changed(before);
}

void test_sda_input(int input)
{
    unsigned before = bus();
    /* Real board function releases the output latch before switching mode. */
    sda_release = 1;
    sda_input_mode = input;
    changed(before);
}

int test_scl_read(void) { return (bus() & 1u) != 0u; }
int test_sda_read(void) { return (bus() & 2u) != 0u; }
uint8_t test_bus_lines(void) { return (uint8_t)bus(); }
uint16_t test_time_us(void) { return (uint16_t)++ticks; }
void test_delay_us(uint16_t us) { ticks += us; }

static void reset_model(unsigned addr)
{
    phase = RX_BITS;
    bits = byte = address_byte = selected = reading = ack_wanted = 0u;
    active = ack_seen = read_index = pointer = payload_count = 0u;
    starts = stops = master_ack_count = address_count = 0u;
    memset(addresses, 0, sizeof(addresses));
    scl_release = sda_release = 1;
    sda_input_mode = slave_low = force_scl_low = force_sda_low = 0;
    nack_pointer = nack_read_address = 0;
    ticks = 0u;
    slave_address = addr;
    assert(test_i2c_init() == 0);
}

static void read_ids(unsigned address)
{
    test_i2c_diag_t d;
    uint16_t value = 0u;
    assert(test_i2c_read_reg((uint8_t)address, 0x7Eu, &value) == 0);
    assert(value == 0x5449u);
    assert(addresses[address_count - 2u] == address << 1);
    assert(addresses[address_count - 1u] == (address << 1 | 1u));
    assert(master_ack_count == 2u && master_acks[0] == 0u && master_acks[1] == 1u);
    master_ack_count = 0u;
    assert(test_i2c_read_reg((uint8_t)address, 0x7Fu, &value) == 0);
    assert(value == 0x3055u && master_ack_count == 2u);
    assert(master_acks[0] == 0u && master_acks[1] == 1u);
    test_i2c_get_diag(&d);
    assert(d.rc == 0 && d.stop_rc == 0 && d.stage == TEST_STAGE_DONE);
    assert(d.tx == (address << 1 | 1u) && d.ack_level == 0u && d.clocks == 9u);
    assert(d.hz > 10000u && d.hz <= 100000u);
    assert(bus() == 3u && !active && !slave_low && !sda_input_mode);
}

int main(void)
{
    test_i2c_diag_t d;
    uint16_t value;
    uint32_t before;
    unsigned address, found = 0u;

    reset_model(0x2Au);
    assert(test_i2c_probe(0x2Au) == 0 && addresses[0] == 0x54u);
    assert(starts == 1u && stops == 1u);
    read_ids(0x2Au);
    assert(starts == 5u && stops == 3u); /* Each ID uses a repeated START. */

    reset_model(0x2Bu);
    assert(test_i2c_probe(0x2Au) == TEST_I2C_NACK);
    assert(test_i2c_probe(0x2Bu) == 0);
    read_ids(0x2Bu);

    reset_model(0x2Au);
    for (address = 0x08u; address <= 0x77u; ++address) {
        int rc = test_i2c_probe((uint8_t)address);
        assert(rc == (address == 0x2Au ? 0 : TEST_I2C_NACK));
        assert(addresses[address_count - 1u] == address << 1);
        test_i2c_get_diag(&d);
        assert(d.tx == address << 1 && d.clocks == 9u && d.stop_rc == 0);
        assert(d.ack_level == (rc == 0 ? 0u : 1u));
        assert(bus() == 3u && !active && !sda_input_mode);
        if (rc == 0) ++found;
    }
    assert(found == 1u && address_count == 112u);

    reset_model(0x2Au);
    value = 0xFFFFu;
    assert(test_i2c_read_reg(0x2Bu, 0x7Eu, &value) == TEST_I2C_NACK && value == 0u);
    test_i2c_get_diag(&d);
    assert(d.stage == TEST_STAGE_ADDR_W && d.tx == 0x56u && d.ack_level == 1u);
    nack_pointer = 1;
    assert(test_i2c_read_reg(0x2Au, 0x7Eu, &value) == TEST_I2C_NACK);
    test_i2c_get_diag(&d);
    assert(d.stage == TEST_STAGE_REG && d.tx == 0x7Eu);

    reset_model(0x2Au);
    nack_read_address = 1;
    assert(test_i2c_read_reg(0x2Au, 0x7Eu, &value) == TEST_I2C_NACK);
    test_i2c_get_diag(&d);
    assert(d.stage == TEST_STAGE_ADDR_R && d.tx == 0x55u);

    reset_model(0x2Au);
    ticks = 65520u;
    assert(test_i2c_probe(0x2Au) == 0); /* Timer wraps within an address byte. */
    read_ids(0x2Au);

    reset_model(0x2Au);
    force_scl_low = 1;
    before = ticks;
    assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS);
    test_i2c_get_diag(&d);
    assert(d.stage == TEST_STAGE_START && d.clocks == 0u && d.stop_rc == TEST_I2C_BUS);
    assert(ticks - before >= 4000u && ticks - before < 4500u);
    assert(scl_release && sda_release && !sda_input_mode);

    reset_model(0x2Au);
    force_sda_low = 1;
    assert(test_i2c_init() == TEST_I2C_BUS);
    test_i2c_get_diag(&d);
    assert(d.stop_rc == TEST_I2C_BUS);
    assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS);

    reset_model(0x2Au);
    assert(test_i2c_probe(0x80u) == TEST_I2C_ARGUMENT);
    assert(test_i2c_read_reg(0x2Au, 0x7Eu, 0) == TEST_I2C_ARGUMENT);
    assert(starts == 0u && stops == 0u);

    puts("PASS: NEW I2C driver, address shifts, explicit-input ACK, IDs 5449/3055,");
    puts("      repeated START, final-byte NACK, error stages, STOP, timer wrap,");
    puts("      stuck-line timeouts, recovery and no configuration writes.");
    return 0;
}
