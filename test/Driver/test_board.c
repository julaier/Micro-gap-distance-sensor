/* New minimal board support. Does not call Driver/board.c or uart.c. */
#include "stm32c0xx.h"
#include "stm32c0xx_ll_rcc.h"
#include "stm32c0xx_ll_system.h"
#include "test_board.h"

#define SCL_PIN (1u << 7)
#define SDA_PIN (1u << 6)
#define SDA_MODE_MASK (3u << 12)

static int s_hse;

static void clock_init(void)
{
    uint32_t guard;

    LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
    LL_RCC_HSI_Enable();
    while (LL_RCC_HSI_IsReady() == 0u) { }
    LL_RCC_SetHSIDiv(LL_RCC_HSI_DIV_1);
    LL_RCC_SetAHBPrescaler(LL_RCC_HCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSI);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSI) { }

    LL_RCC_HSE_Enable();
    for (guard = 0u; guard < 2000000u; ++guard) {
        if (LL_RCC_HSE_IsReady() != 0u) break;
    }
    s_hse = LL_RCC_HSE_IsReady() != 0u;
    if (s_hse) {
        LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSE);
        while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSE) { }
        LL_FLASH_SetLatency(LL_FLASH_LATENCY_0);
    }
    SystemCoreClockUpdate();
}

static void timer_init(void)
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
}

static void gpio_uart_init(void)
{
    const uint32_t i2c_modes = (3u << 12) | (3u << 14);

    RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN;
    (void)RCC->IOPENR;

    /* Release both latches before enabling open-drain outputs. */
    GPIOB->BSRR = SCL_PIN | SDA_PIN;
    GPIOB->OTYPER |= SCL_PIN | SDA_PIN;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~i2c_modes) | (1u << 12) | (1u << 14);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~i2c_modes) | (1u << 12) | (1u << 14);
    GPIOB->MODER = (GPIOB->MODER & ~i2c_modes) | (1u << 12) | (1u << 14);

    /* PA4: observe INTB only, with no EXTI or NVIC setup. */
    GPIOA->MODER &= ~(3u << 8);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 8)) | (1u << 8);

    /* PA2/PA3: USART2 AF1. Keep PA13/PA14 SWD untouched. */
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 4) | (3u << 6))) |
                   (2u << 4) | (2u << 6);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((15u << 8) | (15u << 12))) |
                    (1u << 8) | (1u << 12);
    GPIOA->OTYPER &= ~((1u << 2) | (1u << 3));
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3u << 4) | (3u << 6))) |
                   (1u << 4) | (1u << 6);

    RCC->APBENR1 |= RCC_APBENR1_USART2EN;
    (void)RCC->APBENR1;
    RCC->APBRSTR1 |= RCC_APBRSTR1_USART2RST;
    RCC->APBRSTR1 &= ~RCC_APBRSTR1_USART2RST;
    USART2->CR1 = 0u;
    USART2->CR2 = 0u;
    USART2->CR3 = 0u;
    USART2->PRESC = 0u;
    USART2->BRR = (SystemCoreClock + 57600u) / 115200u;
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    while ((USART2->ISR & (USART_ISR_TEACK | USART_ISR_REACK)) !=
           (USART_ISR_TEACK | USART_ISR_REACK)) { }
}

void test_board_init(void)
{
    /* All timing is polled. Do not start SysTick, ADC, OLED, EXTI or watchdog. */
    SysTick->CTRL = 0u;
    clock_init();
    timer_init();
    gpio_uart_init();
}

int test_board_uses_hse(void) { return s_hse; }
uint32_t test_board_clock_hz(void) { return SystemCoreClock; }
uint16_t test_time_us(void) { return (uint16_t)TIM14->CNT; }

void test_delay_us(uint16_t us)
{
    uint16_t start = test_time_us();
    while ((uint16_t)(test_time_us() - start) < us) { }
}

void test_delay_ms(uint16_t ms)
{
    while (ms-- != 0u) test_delay_us(1000u);
}

void test_uart_putc(char c)
{
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0u) { }
    USART2->TDR = (uint8_t)c;
}

void test_uart_puts(const char *s)
{
    while (*s != '\0') test_uart_putc(*s++);
    while ((USART2->ISR & USART_ISR_TC) == 0u) { }
}

void test_scl_write(int release)
{
    GPIOB->BSRR = release ? SCL_PIN : (SCL_PIN << 16);
}

void test_sda_write(int release)
{
    GPIOB->BSRR = release ? SDA_PIN : (SDA_PIN << 16);
}

int test_scl_read(void) { return (GPIOB->IDR & SCL_PIN) != 0u; }
int test_sda_read(void) { return (GPIOB->IDR & SDA_PIN) != 0u; }

void test_sda_input(int input)
{
    /* Latch remains released during either direction transition. */
    GPIOB->BSRR = SDA_PIN;
    GPIOB->MODER = (GPIOB->MODER & ~SDA_MODE_MASK) |
                   (input ? 0u : (1u << 12));
}

uint8_t test_bus_lines(void)
{
    return (uint8_t)((test_scl_read() ? 1u : 0u) | (test_sda_read() ? 2u : 0u));
}

int test_intb_read(void) { return (GPIOA->IDR & (1u << 4)) != 0u; }
