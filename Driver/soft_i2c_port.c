/* Only the I2C GPIO/TIM14 subset of test/Driver/test_board.c.
 * Do not reinitialize the production clock, UART, SysTick, EXTI or ADC.
 */
#include "stm32c0xx.h"
#include "soft_i2c_port.h"

#define SCL_PIN (1u << 7)
#define SDA_PIN (1u << 6)
#define SDA_MODE_MASK (3u << 12)
#define I2C_MODE_MASK ((3u << 12) | (3u << 14))

void i2c_port_init(void)
{
    RCC->APBENR2 |= RCC_APBENR2_TIM14EN;
    (void)RCC->APBENR2;
    RCC->APBRSTR2 |= RCC_APBRSTR2_TIM14RST;
    RCC->APBRSTR2 &= ~RCC_APBRSTR2_TIM14RST;
    TIM14->PSC = SystemCoreClock / 1000000u - 1u;
    TIM14->ARR = 0xFFFFu;
    TIM14->EGR = TIM_EGR_UG;
    TIM14->CNT = 0u;
    TIM14->SR = 0u;
    TIM14->CR1 = TIM_CR1_CEN;

    RCC->IOPENR |= RCC_IOPENR_GPIOBEN;
    (void)RCC->IOPENR;
    GPIOB->BSRR = SCL_PIN | SDA_PIN;
    GPIOB->OTYPER |= SCL_PIN | SDA_PIN;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~I2C_MODE_MASK) | (1u << 12) | (1u << 14);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~I2C_MODE_MASK) | (1u << 12) | (1u << 14);
    GPIOB->MODER = (GPIOB->MODER & ~I2C_MODE_MASK) | (1u << 12) | (1u << 14);
}

uint16_t i2c_port_time_us(void) { return (uint16_t)TIM14->CNT; }

void i2c_port_delay_us(uint16_t us)
{
    uint16_t start = i2c_port_time_us();
    while ((uint16_t)(i2c_port_time_us() - start) < us) { }
}

void i2c_port_scl_write(int release)
{
    GPIOB->BSRR = release ? SCL_PIN : (SCL_PIN << 16);
}

void i2c_port_sda_write(int release)
{
    GPIOB->BSRR = release ? SDA_PIN : (SDA_PIN << 16);
}

int i2c_port_scl_read(void) { return (GPIOB->IDR & SCL_PIN) != 0u; }
int i2c_port_sda_read(void) { return (GPIOB->IDR & SDA_PIN) != 0u; }

void i2c_port_sda_input(int input)
{
    GPIOB->BSRR = SDA_PIN;
    GPIOB->MODER = (GPIOB->MODER & ~SDA_MODE_MASK) | (input ? 0u : (1u << 12));
}

uint8_t i2c_port_lines(void)
{
    return (uint8_t)((i2c_port_scl_read() ? 1u : 0u) |
                     (i2c_port_sda_read() ? 2u : 0u));
}

uint8_t i2c_port_sda_mode(void) { return (uint8_t)((GPIOB->MODER >> 12) & 3u); }
uint8_t i2c_port_sda_od(void) { return (GPIOB->OTYPER & SDA_PIN) != 0u ? 1u : 0u; }

uint8_t i2c_port_pull_test(void)
{
    uint8_t down, up;
    /* Only called by post-failure diagnostics, while the bus is idle. */
    GPIOB->BSRR = SCL_PIN | SDA_PIN;
    GPIOB->MODER &= ~I2C_MODE_MASK;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~I2C_MODE_MASK) | (2u << 12) | (2u << 14);
    i2c_port_delay_us(50u);
    down = i2c_port_lines();
    GPIOB->PUPDR = (GPIOB->PUPDR & ~I2C_MODE_MASK) | (1u << 12) | (1u << 14);
    i2c_port_delay_us(50u);
    up = i2c_port_lines();
    GPIOB->MODER = (GPIOB->MODER & ~I2C_MODE_MASK) | (1u << 12) | (1u << 14);
    return (uint8_t)(down | (uint8_t)(up << 2));
}
