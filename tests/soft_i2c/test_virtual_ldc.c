/* An independent open-drain slave model. ACK is derived from the byte
 * received on SCL rising edges, not preloaded as a sample sequence.
 * Compile with Driver/hard_i2c.c and the same stubs as test_soft_i2c.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_gpio.h"
#include "stm32c0xx_ll_tim.h"
#include "hard_i2c.h"
#include "board.h"

static uint32_t slave_scl, slave_sda;
#define SCL slave_scl
#define SDA slave_sda
#define PINS ((1u << 7) | (1u << 6))

GPIO_TypeDef test_gpio_b;
TIM_TypeDef test_tim14;
uint32_t SystemCoreClock = 8000000u;
uint32_t test_timer_us;
uint32_t test_timer_ticks, test_timer_step = 1u;
uint32_t test_irq_mask;
unsigned test_irq_disable_count, test_irq_restore_count;
static uint32_t io_ticks;
static uint32_t master = PINS;
static uint32_t latch = PINS, output_modes = PINS;
static int slave_low, active, address_byte, selected, reading;
static unsigned bits, byte, ack_seen, data_index, read_index;
static unsigned starts, stops, address_count, nack_count;
static unsigned master_ack[16], master_ack_count;
static uint8_t addresses[256], pointer, high_byte;
static uint16_t registers[128];
static uint32_t elapsed_us;
enum phase { RX_BITS, RX_ACK, TX_BITS, TX_ACK, WAIT_STOP };
static enum phase phase;

static uint32_t bus(void)
{
    return master & (slave_low ? ~SDA : ~0u);
}

static uint8_t transmit_value(void)
{
    uint16_t value = registers[pointer];
    return (uint8_t)((read_index & 1u) ? value : (value >> 8));
}

static void transmit_bit(void)
{
    slave_low = (transmit_value() & (0x80u >> bits)) ? 0 : 1;
}

static void rising_edge(void)
{
    unsigned level = (bus() & SDA) ? 1u : 0u;
    if (!active) return;
    switch (phase) {
    case RX_BITS:
        byte = (byte << 1) | level;
        if (++bits == 8) {
            if (address_byte) {
                assert(address_count < sizeof(addresses));
                addresses[address_count++] = (uint8_t)byte;
                selected = (byte >> 1) == 0x2A;
                reading = (byte & 1u) != 0;
                if (!selected) nack_count++;
            } else if (selected) {
                if (data_index == 0) pointer = (uint8_t)byte;
                else if (data_index == 1) high_byte = (uint8_t)byte;
                else if (data_index == 2) {
                    registers[pointer] = (uint16_t)((high_byte << 8) | byte);
                }
                data_index++;
            }
            phase = RX_ACK;
            ack_seen = 0;
        }
        break;
    case RX_ACK:
        assert(level == (selected ? 0u : 1u));
        ack_seen = 1;
        break;
    case TX_BITS:
        if (++bits == 8) { phase = TX_ACK; ack_seen = 0; }
        break;
    case TX_ACK:
        assert(master_ack_count < sizeof(master_ack) / sizeof(master_ack[0]));
        master_ack[master_ack_count++] = level;
        ack_seen = 1;
        break;
    default: break;
    }
}

static void falling_edge(void)
{
    if (!active) return;
    switch (phase) {
    case RX_ACK:
        if (!ack_seen) {
            slave_low = selected;
        } else {
            slave_low = 0;
            bits = byte = 0;
            if (!selected) phase = WAIT_STOP;
            else if (address_byte && reading) {
                phase = TX_BITS;
                read_index = 0;
                transmit_bit();
            } else phase = RX_BITS;
            address_byte = 0;
        }
        break;
    case TX_BITS:
        transmit_bit();
        break;
    case TX_ACK:
        if (!ack_seen) slave_low = 0;
        else if (master_ack[master_ack_count - 1] != 0) phase = WAIT_STOP;
        else {
            read_index++;
            bits = 0;
            phase = TX_BITS;
            transmit_bit();
        }
        break;
    default: break;
    }
}

static void master_change(uint32_t next)
{
    uint32_t before = bus(), after;
    master = next;
    after = bus();
    if ((before & SCL) && (after & SCL) && ((before ^ after) & SDA)) {
        if (after & SDA) {
            stops++;
            active = slave_low = 0;
        } else {
            starts++;
            active = address_byte = 1;
            selected = reading = 0;
            bits = byte = ack_seen = data_index = 0;
            phase = RX_BITS;
        }
    }
    if (!(before & SCL) && (after & SCL)) rising_edge();
    if ((before & SCL) && !(after & SCL)) falling_edge();
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
    assert(port == GPIOB && !(pins & ~PINS));
    test_advance_ticks(io_ticks);
    latch |= pins;
    master_change(latch | (PINS & ~output_modes));
}
void LL_GPIO_ResetOutputPin(GPIO_TypeDef *port, uint32_t pins)
{
    assert(port == GPIOB && !(pins & ~PINS));
    test_advance_ticks(io_ticks);
    assert((pins & output_modes) == pins);
    latch &= ~pins;
    master_change(latch | (PINS & ~output_modes));
}
uint32_t LL_GPIO_IsInputPinSet(const GPIO_TypeDef *port, uint32_t pins)
{
    assert(port == GPIOB);
    test_advance_ticks(io_ticks);
    return (bus() & pins) == pins;
}
uint32_t LL_GPIO_ReadInputPort(const GPIO_TypeDef *port)
{
    assert(port == GPIOB);
    return bus();
}
void LL_GPIO_Init(GPIO_TypeDef *port, LL_GPIO_InitTypeDef *config)
{
    assert(port == GPIOB && config->Pin == PINS);
    if (config->Mode == LL_GPIO_MODE_OUTPUT) {
        assert(config->OutputType == LL_GPIO_OUTPUT_OPENDRAIN);
        port->OTYPER |= config->Pin;
    }
    LL_GPIO_SetPinMode(port, 1u << 6, config->Mode);
    LL_GPIO_SetPinMode(port, 1u << 7, config->Mode);
}
void LL_GPIO_SetPinMode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode)
{
    unsigned shift = pin == (1u << 6) ? 12u : 14u;
    assert(port == GPIOB && (pin == (1u << 6) || pin == (1u << 7)));
    assert(latch & pin); /* output latch must be released before switching */
    port->MODER = (port->MODER & ~(3u << shift)) | (mode << shift);
    if (mode == LL_GPIO_MODE_OUTPUT) output_modes |= pin;
    else output_modes &= ~pin;
    master_change(latch | (PINS & ~output_modes));
}
uint32_t board_millis(void) { return elapsed_us / 1000u; }
void board_delay_us(uint32_t us) { elapsed_us += us; }

static void run_case(uint8_t swapped, uint8_t input_rx)
{
    uint8_t tx[] = {0x1C, 0x80, 0x00}, rx[2], reg;
    unsigned saved_starts;
    hard_i2c_diag_t diag;
    slave_scl = 1u << (swapped ? 6 : 7);
    slave_sda = 1u << (swapped ? 7 : 6);
    master = latch = output_modes = PINS;
    slave_low = active = address_byte = selected = reading = 0;
    bits = byte = ack_seen = data_index = read_index = 0;
    starts = stops = address_count = nack_count = master_ack_count = 0;
    memset(&test_gpio_b, 0, sizeof(test_gpio_b));
    memset(registers, 0, sizeof(registers));
    registers[0x7E] = 0x5449;
    registers[0x7F] = 0x3055;
    hard_i2c_init();
    hard_i2c_debug_config(swapped, input_rx);
    assert(hard_i2c_write(0x2A, 0, 0) == 0);
    assert(address_count == 1 && addresses[0] == 0x54);
    hard_i2c_get_diag(&diag);
    assert(diag.tx_byte == 0x54 && diag.sampled_byte == 0x54 && diag.ack_samples == 0);
    assert(diag.ack_sda_mode == (input_rx ? 0u : 1u) && diag.ack_sda_od == 1);
    assert(hard_i2c_write(0x2B, 0, 0) == -2);
    assert(addresses[1] == 0x56 && nack_count == 1);
    hard_i2c_get_diag(&diag);
    assert(diag.tx_byte == 0x56 && diag.sampled_byte == 0x56 && diag.ack_samples == 7);
    assert(hard_i2c_write(0x2A, tx, sizeof(tx)) == 0);
    assert(registers[0x1C] == 0x8000);
    reg = 0x7E;
    saved_starts = starts;
    assert(hard_i2c_write_read(0x2A, &reg, 1, rx, 2) == 0);
    assert(starts == saved_starts + 2);
    assert(rx[0] == 0x54 && rx[1] == 0x49);
    assert(master_ack_count == 2 && master_ack[0] == 0 && master_ack[1] == 1);
    assert(addresses[address_count - 2] == 0x54 && addresses[address_count - 1] == 0x55);
    reg = 0x7F;
    assert(hard_i2c_write_read(0x2A, &reg, 1, rx, 2) == 0);
    assert(rx[0] == 0x30 && rx[1] == 0x55);
    assert(master == PINS && !slave_low && !active && stops == 5);
    if (swapped == 0u && input_rx == 0u) {
        /* Calibration must work both with ACK and NACK, without writing a register. */
        io_ticks = 3u;
        assert(hard_i2c_calibrate_100k(0x2A) == 0);
        hard_i2c_get_diag(&diag);
        assert(diag.clocks == 9u && diag.effective_hz >= 80000u && diag.effective_hz <= 125000u);
        assert(registers[0x1C] == 0x8000u);
        printf("PASS: ACK calibration, GPIO overhead 0.75us/call, measured model %lu Hz.\n",
               (unsigned long)diag.effective_hz);
        assert(hard_i2c_calibrate_100k(0x2B) == -2);
        hard_i2c_get_diag(&diag);
        assert(diag.clocks == 9u && diag.effective_hz >= 80000u && diag.effective_hz <= 125000u);
        assert(diag.ack_samples == 7u);
        assert(hard_i2c_write_read(0x2A, &reg, 1, rx, 2) == 0);
        assert(rx[0] == 0x30 && rx[1] == 0x55 && test_irq_mask == 0u);
        io_ticks = 0u;
    }
    assert(master == PINS && !slave_low && !active);
    /* A deliberately wrong mapping must not be accepted by a fake ACK path.
     * Restore the matching mapping and check that the device is still readable. */
#if HARD_I2C_DIAGNOSTIC_MATRIX
    hard_i2c_debug_config((uint8_t)!swapped, input_rx);
    assert(hard_i2c_write(0x2A, 0, 0) != 0);
    hard_i2c_debug_config(swapped, input_rx);
#endif
    assert(hard_i2c_write_read(0x2A, &reg, 1, rx, 2) == 0);
    assert(rx[0] == 0x30 && rx[1] == 0x55);
    {
        unsigned address, found = 0u;
        unsigned previous_nacks = nack_count;
        for (address = 0x08u; address <= 0x77u; address++) {
            int expected_rc = address == 0x2Au ? 0 : -2;
            assert(hard_i2c_write((uint8_t)address, 0, 0u) == expected_rc);
            hard_i2c_get_diag(&diag);
            assert(addresses[address_count - 1u] == (uint8_t)(address << 1));
            assert(diag.tx_byte == (uint8_t)(address << 1));
            assert(diag.sampled_byte == diag.tx_byte && diag.clocks == 9u);
            assert(diag.ack_samples == (expected_rc == 0 ? 0u : 7u));
            if (expected_rc == 0) found++;
            assert(master == PINS && !slave_low && !active);
        }
        assert(found == 1u && nack_count - previous_nacks == 111u);
        assert(registers[0x1C] == 0x8000 && registers[0x7E] == 0x5449);
    }
    printf("PASS: SCL PB%u / SDA PB%u, receive %s, IDs 5449/3055.\n",
           swapped ? 6u : 7u, swapped ? 7u : 6u, input_rx ? "INPUT" : "OD");
}

int main(void)
{
    run_case(0, 0);
    run_case(0, 1);
#if HARD_I2C_DIAGNOSTIC_MATRIX
    run_case(1, 0);
    run_case(1, 1);
#endif
    return 0;
}
