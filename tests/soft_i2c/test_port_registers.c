/* Check the actual MCU port implementation against simple volatile registers.
 * This verifies masks and initialization, not analog GPIO behavior or timing.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32c0xx.h"
#include "soft_i2c_port.h"

RCC_TypeDef port_test_rcc;
GPIO_TypeDef port_test_gpio_b;
TIM_TypeDef port_test_tim14;
uint32_t SystemCoreClock;

static void check_clock(uint32_t hz)
{
    const uint32_t mask = (3u << 12) | (3u << 14);
    uint32_t saved_mode, saved_pulls, saved_speed;
    memset(&port_test_rcc, 0, sizeof(port_test_rcc));
    memset(&port_test_tim14, 0xFF, sizeof(port_test_tim14));
    memset(&port_test_gpio_b, 0xA5, sizeof(port_test_gpio_b));
    saved_mode = GPIOB->MODER;
    saved_pulls = GPIOB->PUPDR;
    saved_speed = GPIOB->OSPEEDR;
    RCC->APBENR2 = 0x10u;
    RCC->APBRSTR2 = 0x10u;
    RCC->IOPENR = 0x01u;
    SystemCoreClock = hz;
    i2c_port_init();
    assert(RCC->APBENR2 == (0x10u | (1u << 15)));
    assert(RCC->APBRSTR2 == 0x10u && RCC->IOPENR == 3u);
    assert(TIM14->PSC == hz / 1000000u - 1u && TIM14->ARR == 0xFFFFu);
    assert(TIM14->EGR == 1u && TIM14->CNT == 0u && TIM14->SR == 0u && TIM14->CR1 == 1u);
    assert(GPIOB->BSRR == 0xC0u && (GPIOB->OTYPER & 0xC0u) == 0xC0u);
    assert(GPIOB->MODER == ((saved_mode & ~mask) | (1u << 12) | (1u << 14)));
    assert(GPIOB->PUPDR == ((saved_pulls & ~mask) | (1u << 12) | (1u << 14)));
    assert(GPIOB->OSPEEDR == ((saved_speed & ~mask) | (1u << 12) | (1u << 14)));
    i2c_port_scl_write(0); assert(GPIOB->BSRR == 0x00800000u);
    i2c_port_scl_write(1); assert(GPIOB->BSRR == 0x00000080u);
    i2c_port_sda_write(0); assert(GPIOB->BSRR == 0x00400000u);
    i2c_port_sda_write(1); assert(GPIOB->BSRR == 0x00000040u);
    saved_mode = GPIOB->MODER;
    i2c_port_sda_input(1);
    assert(GPIOB->BSRR == 0x40u && GPIOB->MODER == (saved_mode & ~(3u << 12)));
    assert(i2c_port_sda_mode() == 0u && i2c_port_sda_od() == 1u);
    i2c_port_sda_input(0);
    assert(GPIOB->MODER == saved_mode && i2c_port_sda_mode() == 1u);
    for (unsigned lines = 0u; lines < 4u; ++lines) {
        GPIOB->IDR = (lines & 1u ? 1u << 7 : 0u) | (lines & 2u ? 1u << 6 : 0u);
        assert(i2c_port_lines() == lines);
    }
    TIM14->CNT = 65530u;
    assert(i2c_port_time_us() == 65530u);
}

int main(void)
{
    check_clock(8000000u);
    check_clock(12000000u);
    check_clock(48000000u);
    puts("PASS: actual port GPIO PB7/PB6 masks, input ACK mode, unrelated-bit preservation and TIM14 1MHz setup.");
    return 0;
}
