/*
 * board.c - 板级基础服务实现
 *
 * 时钟: 内部HSI直接8MHz，不启用HSE或PLL
 *       AHB/APB1/APB2 = 8MHz, ADC = 4MHz
 * 时基: SysTick 1ms
 * INTB: PB0 / EXTI0 下降沿
 *
 * 本文件统一使用 STM32F10x Standard Peripheral Library API。
 */

#include "stm32f10x.h"
#include "board.h"

/* 1ms 时基: 按实际系统时钟算, 退回 HSI 8MHz 时也是准的 */
#define SYSTICK_HZ   (SystemCoreClock / 1000u)

/* 调试期先关看门狗, 量产打开时改这里 */
#define BOARD_USE_IWDG   0

static volatile uint32_t s_tick_ms;
static volatile uint8_t  s_intb_flag;
// 系统滴答中断, 1ms
void SysTick_Handler(void)
{
    s_tick_ms++;
}
// 返回系统运行毫秒数，溢出约 49.7 天。
uint32_t board_millis(void)
{
    return s_tick_ms;
}
// 延时 ms 毫秒，阻塞式
void board_delay_ms(uint32_t ms)
{
    uint32_t t0 = s_tick_ms;
    while ((uint32_t)(s_tick_ms - t0) < ms) {
    }
}
//延迟us，阻塞
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
            /* Only for a call before SysTick init; normal calls use counter. */
            volatile uint32_t guard;
            for (guard = 0u; guard <= ticks_per_us; guard++) {}
        }
    }
}
// EXTI0 中断服务函数, PB0 下降沿触发
void EXTI0_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line0) != RESET) {
        EXTI_ClearITPendingBit(EXTI_Line0);
        s_intb_flag = 1u;
    }
}
/*ldc1614通道转换的时候触发中断，更新数据*/
int board_intb_take(void)
{
    int f;

    __disable_irq();
    f = (int)s_intb_flag;
    s_intb_flag = 0u;
    __enable_irq();
    return f;
}
//系统时钟当前是不是 72MHz (外部晶振没起来会退到内部 HSI 8MHz)
int board_clock_is_72m(void)
{
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);
    return (clocks.SYSCLK_Frequency == 72000000u &&
            RCC_GetSYSCLKSource() == 0x08u &&
            (RCC->CFGR & RCC_CFGR_PLLSRC) != 0u &&
            RCC_GetFlagStatus(RCC_FLAG_HSERDY) != RESET &&
            RCC_GetFlagStatus(RCC_FLAG_PLLRDY) != RESET) ? 1 : 0;
}
/* This variant never attempts an external oscillator or PLL. */

// HSI8MHz初始化
static void clock_init(void)
{
    RCC_DeInit();
    RCC_HSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_HSIRDY) == RESET) {}
    RCC_SYSCLKConfig(RCC_SYSCLKSource_HSI);
    while (RCC_GetSYSCLKSource() != 0x00u) {}
    RCC_PLLCmd(DISABLE);
    RCC_HSEConfig(RCC_HSE_OFF);
    RCC_HCLKConfig(RCC_SYSCLK_Div1);
    RCC_PCLK1Config(RCC_HCLK_Div1);
    RCC_PCLK2Config(RCC_HCLK_Div1);
    RCC_ADCCLKConfig(RCC_PCLK2_Div2);
    FLASH_SetLatency(FLASH_Latency_0);
    FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);
    SystemCoreClockUpdate();
}
// INTB 中断初始化, PB0, 下降沿触发
static void intb_init(void)
{
    GPIO_InitTypeDef gpio;
    EXTI_InitTypeDef exti;
    NVIC_InitTypeDef nvic;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);

    /* PB0: 输入上拉，INTB 是推挽输出，内部上拉只是兜底。 */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_0;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);

    GPIO_EXTILineConfig(GPIO_PortSourceGPIOB, GPIO_PinSource0);

    EXTI_StructInit(&exti);
    exti.EXTI_Line    = EXTI_Line0;
    exti.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Falling;   /* INTB 低有效 */
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);

    nvic.NVIC_IRQChannel                   = EXTI0_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1u;
    nvic.NVIC_IRQChannelSubPriority        = 1u;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
}
//喂看门狗，防止复位
void board_feed_watchdog(void)
{
#if BOARD_USE_IWDG
    IWDG_ReloadCounter();
#endif
}
// 复位系统
void board_reset(void)
{
    NVIC_SystemReset();
}
// 上电初始化: HSI8MHz -> 1ms时基 -> INTB中断。
void board_init(void)
{
    clock_init();

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    (void)SysTick_Config(SYSTICK_HZ);
    intb_init();
//开看门狗，2s 超时，如果想1.25s超时，IWDG_SetReload(781u);
#if BOARD_USE_IWDG
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(1250u);
    IWDG_ReloadCounter();
    IWDG_Enable();
#endif
}

int board_clock_is_hsi8(void)
{
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return (clocks.SYSCLK_Frequency == 8000000u &&
            RCC_GetSYSCLKSource() == 0x00u &&
            RCC_GetFlagStatus(RCC_FLAG_HSIRDY) != RESET &&
            (RCC->CR & (RCC_CR_HSEON | RCC_CR_PLLON)) == 0u) ? 1 : 0;
}
