/* Real margin driver + real F103 LDC implementation/adapter/startup.
 * Only GPIO, timer and UART are simulated. Edges, not API calls, decode the
 * slave frames. One model tick is an 8MHz CPU cycle (1/8 microsecond).
 * This is digital verification, not evidence of analog board waveforms. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "margin_i2c.h"
#include "margin_port.h"
#include "flow_bus.h"
#include "f103_flow.h"
#include "ldc1614.h"

enum { RX, RX_ACK, TX, TX_ACK, WAIT_STOP };
static uint32_t cycles, cost, scl_ready, sda_ready, slave_ready;
static unsigned scl, sda, scl_release, sda_release, slave_low, slave_next;
static unsigned phase, active, is_address, reading, selected, acked, ack_wanted;
static unsigned bits, byte_value, pointer, high_byte, payload, read_index, slot;
static unsigned starts, stops, rises, tx_count, txn, slave_address, master_acks;
static unsigned sent[256], writes, regs_written[128], values_written[128];
static uint16_t regs[128];
static int nack_txn, nack_slot, force_scl, force_sda, stop_fault;
static unsigned scl_delay, sda_delay, slave_delay, sample_calls, ack_sample;
static int ack_pattern, scl_pattern;
static uint32_t irq_mask, locks, unlocks;
static uint32_t last_fall, last_rise, last_sda, start_at, stop_at;
static unsigned had_fall, had_rise, had_sda, had_stop, min_low, min_high;
static unsigned check_timing, checking_start, first_low, output_n;
static char output[32768];

static void defaults(void)
{
    memset(regs, 0, sizeof(regs));
    regs[0x7E] = 0x5449; regs[0x7F] = 0x3055;
    regs[0x1A] = 0x2801; regs[0x1B] = 0x020F;
    regs[0x08] = regs[0x09] = 0x0080;
}
static unsigned tx_bit(void)
{
    uint16_t v = regs[pointer & 127u];
    unsigned b = read_index ? (v & 255u) : (v >> 8);
    return (b & (0x80u >> bits)) == 0u;
}
static void drive_slave(unsigned low)
{
    slave_next = low; slave_ready = cycles + slave_delay;
}
static void rise(void)
{
    ++rises;
    if (check_timing && had_fall) {
        unsigned low = cycles - last_fall;
        assert(low >= MARGIN_LOW_US * 8u);
        if (low < min_low) min_low = low;
        if (had_sda) assert(cycles - last_sda >= 8u); /* >=1us, versus 0.1us. */
    }
    last_rise = cycles; had_rise = 1u;
    if (!active) return;
    if (phase == RX) {
        byte_value = (byte_value << 1) | sda;
        if (++bits == 8u) {
            assert(tx_count < 256u); sent[tx_count++] = byte_value;
            ack_wanted = 1u;
            if (is_address) {
                reading = byte_value & 1u;
                selected = (byte_value >> 1) == slave_address;
                ack_wanted = selected;
            } else if (!selected) ack_wanted = 0u;
            if ((int)txn == nack_txn && (int)slot == nack_slot) ack_wanted = 0u;
            if (!is_address && ack_wanted) {
                if (payload == 0u) pointer = byte_value;
                else if (payload == 1u) high_byte = byte_value;
                else {
                    unsigned v = (high_byte << 8) | byte_value;
                    assert(payload == 2u && writes < 128u);
                    regs_written[writes] = pointer; values_written[writes++] = v;
                    if (pointer == 0x1Cu) { assert(v == 0x8000u); defaults(); }
                    else regs[pointer & 127u] = (uint16_t)v;
                }
                ++payload;
            }
            ++slot; phase = RX_ACK; acked = 0u;
        }
    } else if (phase == RX_ACK) {
        assert(sda_release); /* Master must not synthesize its own ACK. */
        assert(sda == (ack_wanted ? 0u : 1u));
        acked = 1u; ack_sample = 0u;
    } else if (phase == TX) {
        assert(sda_release);
        if (++bits == 8u) { phase = TX_ACK; acked = 0u; }
    } else if (phase == TX_ACK) {
        assert(sda == (read_index ? 1u : 0u));
        ++master_acks; acked = 1u;
    }
}
static void fall(void)
{
    if (check_timing && had_rise && !checking_start) {
        unsigned high = cycles - last_rise;
        assert(high >= MARGIN_HIGH_US * 8u);
        if (high < min_high) min_high = high;
    }
    if (checking_start) {
        assert(cycles - start_at >= MARGIN_START_US * 8u);
        checking_start = 0u; first_low = 1u;
    }
    last_fall = cycles; had_fall = 1u;
    if (!active) return;
    if (phase == RX_ACK) {
        if (!acked) drive_slave(ack_wanted);
        else {
            drive_slave(0u); bits = byte_value = 0u;
            if (!ack_wanted) phase = WAIT_STOP;
            else if (is_address && reading) {
                phase = TX; read_index = 0u; drive_slave(tx_bit());
            } else phase = RX;
            is_address = 0u;
        }
    } else if (phase == TX) drive_slave(tx_bit());
    else if (phase == TX_ACK) {
        if (!acked) drive_slave(0u);
        else if (read_index) phase = WAIT_STOP;
        else { ++read_index; bits = 0u; phase = TX; drive_slave(tx_bit()); }
    }
}
static void update(void)
{
    unsigned old_scl = scl, old_sda = sda;
    if (cycles >= slave_ready) slave_low = slave_next;
    scl = scl_release && !force_scl && cycles >= scl_ready;
    sda = sda_release && !force_sda && !slave_low && cycles >= sda_ready;
    if (old_scl && scl && old_sda != sda) {
        if (!sda) {
            if (check_timing) {
                if (active) assert(cycles - last_rise >= MARGIN_START_US * 8u);
                else if (had_stop) assert(cycles - stop_at >= MARGIN_FREE_US * 8u);
            }
            if (!active) { ++txn; slot = 0u; }
            ++starts; active = is_address = checking_start = 1u;
            start_at = cycles; first_low = 0u;
            selected = reading = bits = byte_value = payload = acked = 0u; phase = RX;
        } else {
            assert(!check_timing || cycles - last_rise >= MARGIN_STOP_US * 8u);
            ++stops; active = 0u; stop_at = cycles; had_stop = 1u;
            slave_low = slave_next = 0u;
        }
    }
    if (!old_scl && scl) rise();
    if (old_scl && !scl) fall();
}
static void step(unsigned count)
{
    while (count--) { ++cycles; update(); }
}
uint16_t margin_now(void) { step(cost); return (uint16_t)(cycles / 8u); }
uint8_t margin_pins(void)
{
    unsigned pins;
    step(cost); pins = scl | (sda << 1);
    if (active && phase == RX_ACK && acked && scl && ack_sample < 3u) {
        /* clock_high observes SCL once first. Count only the later three
         * samples at least 5us after the observed physical rising edge. */
        if (cycles - last_rise >= 5u * 8u) {
            if (ack_pattern >= 0) pins = (pins & ~2u) | ((((unsigned)ack_pattern >> ack_sample) & 1u) << 1);
            if (scl_pattern >= 0) pins = (pins & ~1u) | (((unsigned)scl_pattern >> ack_sample) & 1u);
            ++ack_sample; ++sample_calls;
        }
    }
    return (uint8_t)pins;
}
void margin_scl(int release)
{
    step(cost);
    if (release && !scl_release) scl_ready = cycles + scl_delay;
    scl_release = release != 0; update();
}
void margin_sda(int release)
{
    unsigned old = sda_release;
    step(cost);
    if (old != (unsigned)(release != 0) && !scl && check_timing && had_fall) {
        assert(cycles - last_fall >= MARGIN_HOLD_US * 8u);
        /* Address MSB is zero: no SDA release/reassert glitch after START. */
        if (first_low && active && is_address && bits == 0u) assert(!release);
    }
    if (release && !old) sda_ready = cycles + sda_delay;
    if (release && !old && stop_fault && scl) force_sda = 1;
    sda_release = release != 0;
    update();
    if (!scl && old != sda_release) { last_sda = cycles; had_sda = 1u; }
}
uint32_t margin_lock(void) { uint32_t p = irq_mask; irq_mask = 1u; ++locks; return p; }
void margin_unlock(uint32_t p) { irq_mask = p; ++unlocks; }
uint16_t test_time_us(void) { return margin_now(); }
void test_delay_us(uint16_t us) { step((unsigned)us * 8u); }
void test_delay_ms(uint16_t ms) { step((unsigned)ms * 8000u); }
uint8_t test_bus_lines(void) { return (uint8_t)(scl | (sda << 1)); }
int test_intb_read(void) { return 1; }
void test_uart_putc(char c)
{
    assert(!active && output_n + 1u < sizeof(output));
    output[output_n++] = c; output[output_n] = 0;
}
void test_uart_puts(const char *s) { while (*s) test_uart_putc(*s++); }

static void reset(void)
{
    cycles = scl_ready = sda_ready = slave_ready = 0u; cost = 2u;
    scl = sda = scl_release = sda_release = 1u; slave_low = slave_next = 0u;
    phase = RX; active = is_address = reading = selected = acked = ack_wanted = 0u;
    bits = byte_value = pointer = high_byte = payload = read_index = slot = 0u;
    starts = stops = rises = tx_count = txn = writes = master_acks = 0u;
    slave_address = 0x2Au; force_scl = force_sda = stop_fault = 0;
    scl_delay = 16u; sda_delay = 8u; slave_delay = 7u;
    nack_txn = nack_slot = ack_pattern = scl_pattern = -1;
    sample_calls = ack_sample = locks = unlocks = irq_mask = 0u;
    last_fall = last_rise = last_sda = start_at = stop_at = 0u;
    had_fall = had_rise = had_sda = had_stop = checking_start = first_low = 0u;
    min_low = min_high = 0xFFFFFFFFu; check_timing = 1u;
    output_n = 0u; output[0] = 0;
    memset((void *)&g_margin_first_error, 0, sizeof(g_margin_first_error));
    defaults();
}
static void check_idle(void)
{
    assert(!active && scl && sda && irq_mask == 0u && locks == unlocks);
    assert(g_margin_last.low_min >= MARGIN_LOW_US);
    assert(g_margin_last.high_min >= MARGIN_HIGH_US);
    assert(g_margin_last.period_min >= 3u && g_margin_last.period_max <= 100u);
}
static void edge_tests(void)
{
    uint8_t index; uint16_t value; ack_trace_t a; test_i2c_diag_t d;
    unsigned c, i;
    /* Slow input rise, slave output up to 0.875us, timer wrap and different
     * register-access costs. No ACK changes may occur on a ninth high. */
    for (c = 1u; c <= 8u; c *= 2u) {
        reset(); cost = c; cycles = 65520u * 8u;
        assert(test_i2c_init() == 0);
        assert(wake_i2c_write_reg(0x2Au, 0x08u, 0xA55Au, &index) == 0);
        assert(index == 2u && writes == 1u && regs[8] == 0xA55Au);
        assert(tx_count == 4u && sent[0] == 0x54u && sent[1] == 8u && sent[2] == 0xA5u && sent[3] == 0x5Au);
        assert(starts == 1u && stops == 1u && rises == 37u);
        assert(g_margin_last.clocks == 36u);
        ack_i2c_get_trace(&a);
        assert(a.count == 4u);
        for (i = 0u; i < 4u; ++i) {
            assert(a.bytes[i].samples == 3u && a.bytes[i].sda_mask == 0u && a.bytes[i].scl_mask == 7u);
            assert(a.bytes[i].clocks == 9u && a.bytes[i].dt_us[0] >= 5u);
            assert(a.bytes[i].dt_us[1] > a.bytes[i].dt_us[0] && a.bytes[i].dt_us[2] > a.bytes[i].dt_us[1]);
            assert(a.bytes[i].high_us > a.bytes[i].dt_us[2]);
        }
        check_idle();
        assert(test_i2c_read_reg(0x2Au, 8u, &value) == 0 && value == 0xA55Au);
        assert(starts == 3u && stops == 2u && master_acks == 2u && tx_count == 7u);
        assert(sent[4] == 0x54u && sent[5] == 8u && sent[6] == 0x55u);
        assert(g_margin_last.clocks == 45u); check_idle();
    }
    for (i = 0u; i < 4u; ++i) {
        reset(); nack_txn = 1; nack_slot = (int)i;
        assert(wake_i2c_write_reg(0x2Au, 8u, 0x1234u, &index) == TEST_I2C_NACK);
        assert(index == (i ? i - 1u : 255u) && tx_count == i + 1u && writes == 0u);
        assert(g_margin_last.clocks == (i + 1u) * 9u); check_idle();
        test_i2c_get_diag(&d); assert(d.ack_level == 1u && d.clocks == 9u && d.stop_rc == 0);
    }
    for (i = 0u; i < 3u; ++i) {
        reset(); nack_txn = 1; nack_slot = (int)i; value = 0xFFFFu;
        assert(test_i2c_read_reg(0x2Au, 0x7Eu, &value) == TEST_I2C_NACK && value == 0u);
        assert(master_acks == 0u && tx_count == i + 1u); check_idle();
    }
    for (i = 0u; i < 256u; ++i) {
        uint16_t wanted = (uint16_t)((i << 8) | (i ^ 255u));
        reset();
        assert(wake_i2c_write_reg(0x2Au, 8u, wanted, 0) == 0);
        assert(test_i2c_read_reg(0x2Au, 8u, &value) == 0 && value == wanted);
        check_idle();
    }
    for (i = 1u; i < 7u; ++i) {
        margin_timing_t first;
        reset(); ack_pattern = (int)i;
        assert(test_i2c_probe(0x2Au) == ACK_I2C_UNSTABLE);
        ack_i2c_get_trace(&a); assert(a.bytes[0].sda_mask == i && a.bytes[0].clocks == 9u);
        first = g_margin_first_error; ack_pattern = -1;
        assert(test_i2c_probe(0x2Au) == 0);
        assert(memcmp((const void *)&g_margin_first_error, &first, sizeof(first)) == 0);
    }
    reset(); scl_pattern = 6;
    assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS); check_idle();
    reset(); force_scl = 1; check_timing = 0u;
    assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS && starts == 0u && stops == 0u);
    assert(cycles < 5000u * 8u && irq_mask == 0u && locks == unlocks);
    reset(); scl_delay = MARGIN_TIMEOUT_US * 8u + 80u; check_timing = 0u;
    assert(test_i2c_probe(0x2Au) == TEST_I2C_BUS && starts == 1u && tx_count == 0u);
    assert(cycles < 6000u * 8u && irq_mask == 0u && locks == unlocks);
    reset(); force_sda = 1; check_timing = 0u;
    assert(test_i2c_init() == TEST_I2C_BUS && rises == 0u);
    reset(); stop_fault = 1;
    assert(wake_i2c_write_reg(0x2Au, 8u, 0x1234u, &index) == TEST_I2C_BUS);
    test_i2c_get_diag(&d); assert(d.stage == TEST_STAGE_STOP && d.stop_rc == TEST_I2C_BUS);
    assert(writes == 1u && irq_mask == 0u);
    reset(); irq_mask = 1u;
    assert(test_i2c_probe(0x2Au) == 0 && irq_mask == 1u && locks == unlocks);
    reset();
    assert(test_i2c_read_reg(128u, 0, &value) == TEST_I2C_ARGUMENT && starts == 0u && locks == 0u);
    assert(test_i2c_read_reg(0x2Au, 0, 0) == TEST_I2C_ARGUMENT);
    assert(wake_i2c_write_reg(255u, 0, 0, &index) == TEST_I2C_ARGUMENT && index == 255u);
    puts("PASS margin edges: write/read framing, ninth clocks, master ACK/NACK, reSTART/STOP/tBUF.");
    puts("PASS margin timing: per-edge minima, finite rises, delayed slave, timer wrap, access-cost matrix.");
    puts("PASS margin faults: every TX NACK, six unstable ACK patterns, low SCL sample, stuck lines, STOP error, IRQ restore.");
}
int main(int argc, char **argv)
{
    int rc;
    reset();
    if (argc == 1 || strcmp(argv[1], "edges") == 0) { edge_tests(); return 0; }
    if (strncmp(argv[1], "nack_", 5u) == 0) {
        nack_txn = atoi(argv[1] + 5u) + 1; nack_slot = 0;
    }
    rc = f103_flow_start();
    assert(strstr(output, "$B,TRACE_OVERFLOW,0\r\n") != 0);
    if (nack_txn >= 0) {
        assert(rc < 0 && g_flow_bus.first_failed && txn >= (unsigned)nack_txn);
        assert(g_flow_bus.count == (unsigned)nack_txn);
        assert(g_flow_bus.first_failure.bus.rc == TEST_I2C_NACK);
        assert(g_flow_bus.first_ack.bytes[0].sda_mask == 7u);
    } else {
        static const unsigned expected_regs[] = {0x1C,0x14,0x15,0x10,0x11,0x08,0x09,0x1E,0x1F,0x1B,0x1A};
        static const unsigned expected_values[] = {0x8000,0x1001,0x1001,0x180,0x180,0xFFFF,0xFFFF,0x9000,0x9000,0x820C,0x0A01};
        assert(rc == 0 && !g_flow_bus.first_failed && g_flow_bus.count == 15u && writes == 11u);
        assert(memcmp(regs_written, expected_regs, sizeof(expected_regs)) == 0);
        assert(memcmp(values_written, expected_values, sizeof(expected_values)) == 0);
        assert(g_flow_debug.manufacturer_id == 0x5449u && g_flow_debug.device_id == 0x3055u);
        regs[0x18] = 0x000Cu; regs[0] = 1u; regs[1] = 2u; regs[2] = 3u; regs[3] = 4u;
        f103_flow_poll(); assert(g_flow_debug.new_pairs == 1u && g_flow_debug.valid_pairs == 1u);
    }
    {
        unsigned before = writes;
        flow_bus_debug_t saved = g_flow_bus;
        f103_flow_monitor(1u);
        assert(writes == before && memcmp((const void *)&g_flow_bus, &saved, sizeof(saved)) == 0);
    }
    assert(locks == unlocks && irq_mask == 0u);
    printf("PASS real margin driver + original F103 initialization: %s\n", argv[1]);
    return 0;
}
