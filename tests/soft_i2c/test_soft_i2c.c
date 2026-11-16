/* Host test: check actual driver GPIO edges against I2C wire transactions.
 * Slave samples are scripted; this does not simulate analog rise times.
 * Build from the project root:
 * gcc -std=c99 -Wall -Wextra -Werror -Itests/soft_i2c/stubs -IDriver
 *     tests/soft_i2c/test_soft_i2c.c Driver/hard_i2c.c -o <test.exe>
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_gpio.h"
#include "stm32c0xx_ll_tim.h"
#include "hard_i2c.h"
#include "board.h"

/* Intentionally use physical pin numbers, independent of driver macros. */
#define SCL (1u << 7)
#define SDA (1u << 6)
#define PINS (SCL | SDA)

GPIO_TypeDef test_gpio_b;
TIM_TypeDef test_tim14;
uint32_t SystemCoreClock = 8000000u;
uint32_t test_timer_ticks, test_timer_step = 1u;
uint32_t test_irq_mask;
unsigned test_irq_disable_count, test_irq_restore_count;
static uint32_t output, stuck, ticks, pull;
static unsigned stretch_reads, starts, stops, clocks;
static int trace[1024], samples[128];
static unsigned trace_count, sample_count, sample_index;
static int tracing;
static int stuck_on_receive;
static unsigned stretch_on_clock, stretch_irq_checks, stuck_scl_reads;
static uint32_t expected_irq_mask;
static const char *active_case = "";
static uint32_t io_ticks, last_rise, last_fall, min_high, min_low;

static void change_output(uint32_t value)
{
    uint32_t before = output;
    output = value;
    if (!tracing) return;
    if (!(before & SCL) && (output & SCL)) {
        uint32_t duration = test_timer_ticks - last_fall;
        if (duration < min_low) min_low = duration;
        last_rise = test_timer_ticks;
        assert(trace_count < sizeof(trace) / sizeof(trace[0]));
        trace[trace_count++] = (output & SDA) ? 1 : 0;
        clocks++;
        if (stretch_on_clock == clocks) stretch_reads = 5u;
    }
    if ((before & SCL) && !(output & SCL)) {
        uint32_t duration = test_timer_ticks - last_rise;
        if (duration < min_high) min_high = duration;
        last_fall = test_timer_ticks;
    }
    if ((before & SCL) && (output & SCL) && ((before ^ output) & SDA)) {
        if (output & SDA) {
            trace[trace_count++] = 3; /* STOP */
            stops++;
        } else {
            trace[trace_count++] = 2; /* START */
            starts++;
        }
    }
}

void LL_IOP_GRP1_EnableClock(uint32_t clock)
{
    assert(clock == LL_IOP_GRP1_PERIPH_GPIOB);
}

void LL_APB2_GRP1_EnableClock(uint32_t clock) { assert(clock == LL_APB2_GRP1_PERIPH_TIM14); }
void LL_APB2_GRP1_ForceReset(uint32_t clock)
{
    assert(clock == LL_APB2_GRP1_PERIPH_TIM14);
    memset(&test_tim14, 0, sizeof(test_tim14));
}
void LL_APB2_GRP1_ReleaseReset(uint32_t clock) { assert(clock == LL_APB2_GRP1_PERIPH_TIM14); }

void LL_GPIO_SetOutputPin(GPIO_TypeDef *port, uint32_t pins)
{
    assert(port == GPIOB && (pins & ~PINS) == 0);
    test_advance_ticks(io_ticks);
    change_output(output | pins);
}

void LL_GPIO_ResetOutputPin(GPIO_TypeDef *port, uint32_t pins)
{
    assert(port == GPIOB && (pins & ~PINS) == 0);
    test_advance_ticks(io_ticks);
    change_output(output & ~pins);
}

uint32_t LL_GPIO_IsInputPinSet(const GPIO_TypeDef *port, uint32_t pins)
{
    assert(port == GPIOB);
    test_advance_ticks(io_ticks);
    if (pins == SCL && stretch_reads) {
        if (stretch_on_clock && stretch_reads < 5u) {
            assert(test_irq_mask == expected_irq_mask);
            stretch_irq_checks++;
        }
        stretch_reads--;
        return 0;
    }
    if (pins == SCL && stuck_on_receive && (stuck & SCL)) {
        if (stuck_scl_reads++ > 0u) assert(test_irq_mask == expected_irq_mask);
    }
    if (stuck & pins) return 0;
    if (pins == SDA) {
        if (sample_index >= sample_count) {
            fprintf(stderr, "out of samples in %s\n", active_case);
        }
        assert(sample_index < sample_count);
        return (uint32_t)samples[sample_index++];
    }
    return (output & pins) == pins;
}

uint32_t LL_GPIO_ReadInputPort(const GPIO_TypeDef *port)
{
    assert(port == GPIOB);
    return output & ~stuck;
}

void LL_GPIO_Init(GPIO_TypeDef *port, LL_GPIO_InitTypeDef *config)
{
    assert(port == GPIOB && config->Pin == PINS);
    assert(config->Mode == LL_GPIO_MODE_INPUT ||
           config->Mode == LL_GPIO_MODE_OUTPUT);
    if (config->Mode == LL_GPIO_MODE_OUTPUT) {
        assert(config->OutputType == LL_GPIO_OUTPUT_OPENDRAIN);
        port->OTYPER |= config->Pin;
    }
    LL_GPIO_SetPinMode(port, SCL, config->Mode);
    LL_GPIO_SetPinMode(port, SDA, config->Mode);
    pull = config->Pull;
}

void LL_GPIO_SetPinMode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode)
{
    unsigned shift = pin == SDA ? 12u : 14u;
    assert(port == GPIOB && (pin == SDA || pin == SCL));
    assert(output & pin); /* changing direction must not drive a new low */
    port->MODER = (port->MODER & ~(3u << shift)) | (mode << shift);
    if (stuck_on_receive && pin == SDA && mode == LL_GPIO_MODE_INPUT) stuck = SCL;
}

uint32_t board_millis(void) { return ticks++; }
void board_delay_us(uint32_t us) { assert(us >= 5u); }

static void reset_bus(void)
{
    output = PINS;
    stuck = ticks = stretch_reads = 0;
    starts = stops = clocks = trace_count = 0;
    sample_count = sample_index = 0;
    test_timer_ticks = 0;
    test_timer_step = 1u;
    io_ticks = last_rise = last_fall = 0u;
    min_high = min_low = 0xFFFFFFFFu;
    test_irq_mask = expected_irq_mask = 0u;
    test_irq_disable_count = test_irq_restore_count = 0u;
    stretch_on_clock = stretch_irq_checks = stuck_scl_reads = 0u;
    tracing = 0;
    stuck_on_receive = 0;
    hard_i2c_init();
    assert(output == PINS && pull == LL_GPIO_PULL_UP);
    tracing = 1;
}

static void sample(int bit) { samples[sample_count++] = bit; }
static void read_samples(uint8_t value)
{
    int bit;
    for (bit = 7; bit >= 0; bit--) sample((value >> bit) & 1);
}

static void write_samples(uint8_t value, int ack_level)
{
    read_samples(value); /* actual transmitted bits are sampled from IDR */
    sample(ack_level);
    sample(ack_level);
    sample(ack_level); /* V4 的 ACK 三点采样，末点决定 ACK/NACK */
}

static void expect_event(unsigned *pos, int value)
{
    assert(*pos < trace_count && trace[(*pos)++] == value);
}

static void expect_byte(unsigned *pos, uint8_t value, int ack_level)
{
    int bit;
    for (bit = 7; bit >= 0; bit--) expect_event(pos, (value >> bit) & 1);
    expect_event(pos, ack_level);
}

static void check_done(unsigned pos)
{
    assert(pos == trace_count && sample_index == sample_count);
    assert(output == PINS);
}

static void test_register_write(unsigned input_rx)
{
    const uint8_t data[] = {0x1C, 0x80, 0x00};
    unsigned pos = 0;
    hard_i2c_diag_t diag;
    reset_bus();
    hard_i2c_debug_config(0u, (uint8_t)input_rx);
    sample(1); /* idle SDA before START */
    write_samples(0x54, 0); write_samples(0x1C, 0);
    write_samples(0x80, 0); write_samples(0x00, 0);
    sample(1); /* released SDA after STOP */
    assert(hard_i2c_write(0x2A, data, sizeof(data)) == 0);
    assert(starts == 1 && stops == 1 && clocks == 37);
    expect_event(&pos, 2);
    expect_byte(&pos, 0x54, 1); /* master releases SDA for slave ACK */
    expect_byte(&pos, 0x1C, 1);
    expect_byte(&pos, 0x80, 1);
    expect_byte(&pos, 0x00, 1);
    expect_event(&pos, 0); expect_event(&pos, 3);
    check_done(pos);
    hard_i2c_get_diag(&diag);
    assert(diag.tx_byte == 0 && diag.sampled_byte == 0 && diag.ack_samples == 0);
    assert(diag.ack_sda_od == 1 && diag.ack_sda_mode == (input_rx ? 0u : 1u));
}

static void test_register_read(unsigned length, unsigned input_rx)
{
    const uint8_t reg = 0x7E;
    uint8_t data[2] = {0};
    unsigned pos = 0;
    reset_bus();
    hard_i2c_debug_config(0u, (uint8_t)input_rx);
    sample(1); write_samples(0x54, 0); write_samples(0x7E, 0);
    sample(1); write_samples(0x55, 0); /* repeated START, read address */
    read_samples(0x54);
    if (length == 2) read_samples(0x49);
    sample(1); /* STOP */
    assert(hard_i2c_write_read(0x2A, &reg, 1, data, (uint16_t)length) == 0);
    assert(data[0] == 0x54 && (length == 1 || data[1] == 0x49));
    assert(starts == 2 && stops == 1);
    expect_event(&pos, 2);
    expect_byte(&pos, 0x54, 1);
    expect_byte(&pos, 0x7E, 1);
    expect_event(&pos, 1); expect_event(&pos, 2); /* repeated START setup */
    expect_byte(&pos, 0x55, 1);
    /* Master releases SDA for eight input bits, then ACK except on last byte. */
    expect_byte(&pos, 0xFF, length == 1 ? 1 : 0);
    if (length == 2) expect_byte(&pos, 0xFF, 1);
    expect_event(&pos, 0); expect_event(&pos, 3);
    check_done(pos);
}

static void test_nack_and_probe(void)
{
    const uint8_t data[] = {0x1C, 0x80};
    hard_i2c_diag_t diag;
    reset_bus();
    sample(1); write_samples(0x54, 1); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == -2);
    assert(starts == 1 && stops == 1 && clocks == 10);
    assert(sample_index == sample_count && output == PINS);
    hard_i2c_get_diag(&diag);
    assert(diag.rc == -2 && diag.stage == HARD_I2C_ADDR_W);
    assert(diag.tx_byte == 0x54 && diag.sampled_byte == 0x54 && diag.ack_samples == 7);

    reset_bus();
    sample(1); write_samples(0x54, 0); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    assert(starts == 1 && stops == 1 && clocks == 10);
    hard_i2c_get_diag(&diag);
    assert(diag.rc == 0 && diag.stage == HARD_I2C_DONE);

    reset_bus();
    sample(1); write_samples(0x54, 0); write_samples(0x1C, 1); sample(1);
    assert(hard_i2c_write(0x2A, data, 2) == -2);
    assert(starts == 1 && stops == 1 && clocks == 19);
    assert(sample_index == sample_count && output == PINS);
    hard_i2c_get_diag(&diag);
    assert(diag.rc == -2 && diag.stage == HARD_I2C_WRITE && diag.byte_index == 0);
}

static void test_timeout_and_recovery(void)
{
    hard_i2c_diag_t diag;
    active_case = "timeout SCL";
    reset_bus();
    stuck = SCL;
    assert(hard_i2c_write(0x2A, 0, 0) == -1);
    assert(test_timer_ticks >= 120000 && test_timer_ticks < 160000 && output == PINS);
    hard_i2c_get_diag(&diag);
    assert(diag.rc == -1 && diag.stage == HARD_I2C_START && diag.lines == 2);

    active_case = "stuck SDA";
    reset_bus();
    stuck = SDA;
    assert(hard_i2c_write(0x2A, 0, 0) == -1);
    assert(clocks == 11 && output == PINS); /* failed STOP + 9 pulses + STOP */

    active_case = "stretch SCL";
    reset_bus();
    stretch_reads = 3;
    sample(1); write_samples(0x54, 0); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    assert(stretch_reads == 0 && sample_index == sample_count);

    active_case = "timeout after receive";
    reset_bus();
    hard_i2c_debug_config(0u, 1u);
    stuck_on_receive = 1; /* timeout after SDA was switched to input for ACK */
    sample(1); read_samples(0x54); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == -1);
    assert(((GPIOB->MODER >> 12) & 3u) == LL_GPIO_MODE_OUTPUT);
    assert(sample_index == sample_count && output == PINS);
    assert(test_irq_mask == 0u);
    hard_i2c_get_diag(&diag);
    assert(diag.ack_samples == 1u);
}

static void test_wire_sampling(void)
{
    hard_i2c_diag_t diag;
    reset_bus();
    sample(1); read_samples(0x50); /* deliberately differ from transmitted 0x54 */
    sample(1); sample(1); sample(1); /* NACK three times */
    sample(1); /* STOP's released SDA */
    assert(hard_i2c_write(0x2A, 0, 0) == -2);
    hard_i2c_get_diag(&diag);
    assert(diag.tx_byte == 0x54 && diag.sampled_byte == 0x50 && diag.ack_samples == 7);
    assert(sample_index == sample_count);

    reset_bus();
    sample(1); read_samples(0x54);
    sample(1); sample(1); sample(0); /* 只有高电平末点决定 ACK */
    sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    hard_i2c_get_diag(&diag);
    assert(diag.ack_samples == 3u && sample_index == sample_count);
}

static void test_diagnostics_and_arguments(void)
{
    uint8_t data[2] = {0};
    uint8_t levels[4];
    const uint8_t expected[4] = {3, 2, 1, 3};
    reset_bus();
    hard_i2c_pin_test(levels);
    assert(memcmp(levels, expected, sizeof(levels)) == 0 && output == PINS);

    reset_bus();
    sample(1); sample(1); /* SDA samples; SCL is read directly from output */
    assert(hard_i2c_lines() == 15);
    assert(sample_index == sample_count && pull == LL_GPIO_PULL_UP);

    reset_bus();
    stuck = SDA;
    assert(hard_i2c_lines() == 5); /* bit0/2 = PB7 SCL, bit1/3 = PB6 SDA */

    reset_bus();
    stuck = SCL;
    sample(1); sample(1);
    assert(hard_i2c_lines() == 10);

    reset_bus();
    assert(hard_i2c_write(0x80, 0, 0) == -3);
    assert(hard_i2c_write(0x2A, 0, 1) == -3);
    assert(hard_i2c_write(0x2A, data, 256) == -3);
    assert(hard_i2c_write_read(0x2A, 0, 1, data, 2) == -3);
    assert(hard_i2c_write_read(0x2A, data, 1, 0, 2) == -3);
    assert(hard_i2c_write_read(0x2A, data, 1, data, 0) == -3);
    assert(hard_i2c_write_read(0x2A, data, 1, data, 3) == -3);
    assert(trace_count == 0 && sample_index == 0);
}

static void test_timer_wrap_and_fallback(void)
{
    hard_i2c_diag_t diag;
    reset_bus();
    test_tim14.counter = 65532u;
    sample(1); write_samples(0x54, 0); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    hard_i2c_get_diag(&diag);
    assert(diag.clocks == 9 && diag.period_min_ticks > 0 && diag.period_max_ticks < 400);
    SystemCoreClock = 48000000u;
    reset_bus();
    assert(test_tim14.prescaler == 11u);
    sample(1); write_samples(0x54, 0); sample(1);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    SystemCoreClock = 8000000u;
}

static void test_physical_timing_and_irq_state(void)
{
    unsigned mask;
    for (mask = 0u; mask <= 1u; mask++) {
        reset_bus();
        io_ticks = 3u; /* GPIO execution time, independent of timer polling */
        test_irq_mask = expected_irq_mask = mask;
        stretch_on_clock = 4u;
        sample(1); write_samples(0x54, 0); sample(1);
        assert(hard_i2c_write(0x2A, 0, 0) == 0);
        assert(min_high >= 6u && min_low >= 6u);
        assert(test_irq_mask == mask && stretch_irq_checks != 0u);
        assert(test_irq_disable_count != 0u && test_irq_restore_count != 0u);
    }
    reset_bus();
    stuck = SCL;
    assert(hard_i2c_calibrate_100k(0x2A) == -1);
    assert(test_irq_mask == 0u); /* Failed calibration must not mask interrupts. */
}

int main(void)
{
    test_register_write(0);
    test_register_write(1);
    test_register_read(1, 0);
    test_register_read(2, 0);
    test_register_read(1, 1);
    test_register_read(2, 1);
    test_nack_and_probe();
    test_timeout_and_recovery();
    test_wire_sampling();
    test_diagnostics_and_arguments();
    test_timer_wrap_and_fallback();
    test_physical_timing_and_irq_state();
    puts("PASS: PB7 SCL/PB6 SDA, register write/read, repeated START, ACK/NACK,");
    puts("      probe, clock stretching, timeout/recovery, diagnostics, arguments.");
    return 0;
}
