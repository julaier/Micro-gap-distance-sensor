/*
 * board.c - STM32C011F6U6TR 板级基础服务
 *
 * 时钟: 外部 8 MHz 晶振 HSE 直接作为系统时钟。
 *       STM32C011 没有 PLL。HSE 启动失败时退回 HSI 48 MHz，
 *       供 board_clock_is_hse8() 和串口启动信息报告异常。
 * 时钟/GPIO/USART2/TIM14 直接复用 test/Driver/test_board.c。
 * 时基: 首次读 ID 后才启动 SysTick 1 ms 和 INTB EXTI。
 * INTB: PA4 / EXTI4_15 下降沿。
 */

#include "stm32c0xx.h"
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_exti.h"
#include "stm32c0xx_ll_gpio.h"
#include "stm32c0xx_ll_rcc.h"
#include "stm32c0xx_ll_system.h"
#include "board.h"
#include "../test/Driver/test_board.h"

#define BOARD_USE_IWDG   0

static volatile uint32_t s_tick_ms;
static volatile uint8_t  s_intb_flag;

void SysTick_Handler(void)
{
    s_tick_ms++;
}

uint32_t board_millis(void)
{
    return s_tick_ms;
}

void board_delay_ms(uint32_t ms)
{
    uint32_t t0 = s_tick_ms;

    while ((uint32_t)(s_tick_ms - t0) < ms) {
    }
}

void board_delay_us(uint32_t us)
{
    const uint32_t ticks_per_us = SystemCoreClock / 1000000u;
    const uint32_t period = SysTick->LOAD + 1u;

    while (us-- != 0u) {
        if ((SysTick->CTRL & 1u) != 0u) {
            uint32_t start = SysTick->VAL;
            uint32_t elapsed;

            do {
                uint32_t now = SysTick->VAL;
                elapsed = start >= now ? start - now : start + period - now;
            } while (elapsed < ticks_per_us);
        } else {
            volatile uint32_t guard;
            for (guard = 0u; guard <= ticks_per_us; guard++) {
            }
        }
    }
}

void EXTI4_15_IRQHandler(void)
{
    if (LL_EXTI_IsActiveFallingFlag_0_31(LL_EXTI_LINE_4) != 0u) {
        LL_EXTI_ClearFallingFlag_0_31(LL_EXTI_LINE_4);
        s_intb_flag = 1u;
    }
}

int board_intb_take(void)
{
    int flag;

    __disable_irq();
    flag = (int)s_intb_flag;
    s_intb_flag = 0u;
    __enable_irq();
    return flag;
}

static void intb_init(void)
{
    LL_GPIO_InitTypeDef gpio = {0};
    LL_EXTI_InitTypeDef exti = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

    /* PA4: LDC1614 INTB 输入，推挽输出源；内部上拉作为兜底。 */
    gpio.Pin = LL_GPIO_PIN_4;
    gpio.Mode = LL_GPIO_MODE_INPUT;
    gpio.Speed = LL_GPIO_SPEED_FREQ_LOW;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOA, &gpio);

    LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTA, LL_EXTI_CONFIG_LINE4);
    exti.Line_0_31 = LL_EXTI_LINE_4;
    exti.LineCommand = ENABLE;
    exti.Mode = LL_EXTI_MODE_IT;
    exti.Trigger = LL_EXTI_TRIGGER_FALLING;
    LL_EXTI_Init(&exti);

    NVIC_SetPriority(EXTI4_15_IRQn, 1u);
    NVIC_EnableIRQ(EXTI4_15_IRQn);
}

void board_feed_watchdog(void)
{
#if BOARD_USE_IWDG
    LL_IWDG_ReloadCounter(IWDG);
#endif
}

void board_reset(void)
{
    NVIC_SystemReset();
}

void board_init(void)
{
    /* The same minimal initialization used by the working standalone image.
     * No SysTick or INTB interrupts until the reference ID test has finished. */
    s_tick_ms = 0u;
    s_intb_flag = 0u;
    test_board_init();
}

void board_start_app_services(void)
{
    (void)SysTick_Config(SystemCoreClock / 1000u);
    NVIC_SetPriority(SysTick_IRQn, 3u);
    intb_init();
}

int board_clock_is_hse8(void)
{
    return test_board_uses_hse() && test_board_clock_hz() == 8000000u;
}

int board_clock_is_72m(void)
{
    return 0;
}
