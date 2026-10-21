/*
 * board.c - 板级基础服务实现
 *
 * 时钟: HSE 8MHz -> PLL x9 -> 72MHz
 *       AHB = 72MHz, APB1 = 36MHz, APB2 = 72MHz, ADC = 12MHz
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
    while (us-- > 0u) {
        volatile uint32_t i;
        for (i = 0; i < 18u; i++) {   /* 约 1us @72MHz, 可按需微调 */
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
    return (clocks.SYSCLK_Frequency == 72000000u) ? 1 : 0;
}
/* 试着切到外部 8MHz 晶振 x9 = 72MHz。
 * 0 = 成功; -1 = 晶振没起振或 PLL 没锁上, 此时保持 HSI 8MHz 不变。 */
static int clock_try_hse72(void)
{
    uint32_t guard;

    RCC_HSEConfig(RCC_HSE_ON);

    /* 等晶振起振: 计数等不到就放弃, 不能死等 */
    for (guard = 0u; guard < 200000u; guard++) {
        if (RCC_GetFlagStatus(RCC_FLAG_HSERDY) != RESET) {
            break;
        }
    }
    if (RCC_GetFlagStatus(RCC_FLAG_HSERDY) == RESET) {
        RCC_HSEConfig(RCC_HSE_OFF);
        return -1;/*晶振没起来, 继续用 HSI*/
    }

    /* HSE 起来了再配 PLL: Flash 延迟 -> 总线分频 -> PLL x9 = 72MHz */
    FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);
    FLASH_SetLatency(FLASH_Latency_2);
    RCC_HCLKConfig(RCC_SYSCLK_Div1);
    RCC_PCLK2Config(RCC_HCLK_Div1);
    RCC_PCLK1Config(RCC_HCLK_Div2);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);
    RCC_PLLConfig(RCC_PLLSource_HSE_Div1, RCC_PLLMul_9);
    RCC_PLLCmd(ENABLE);

    for (guard = 0u; guard < 200000u; guard++) {
        if (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) != RESET) {
            break;
        }
    }
    if (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {
        RCC_PLLCmd(DISABLE);
        RCC_HSEConfig(RCC_HSE_OFF);
        return -1;/*PLL 没锁上, 继续用 HSI*/
    }

    RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
    for (guard = 0u; guard < 200000u; guard++) {
        if (RCC_GetSYSCLKSource() == 0x08u) {
            break;
        }
    }
    if (RCC_GetSYSCLKSource() != 0x08u) {
        return -1;
    }

    SystemCoreClockUpdate();
    return 0;
}
//滴答时钟初始化，72MHz
static void clock_init(void)
{
    /* 第一步: 先用内部 HSI 8MHz 起来, 这一步不依赖外部晶振, 保证程序一定能跑 */
    RCC_DeInit();
    RCC_HSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_HSIRDY) == RESET) {
    }
    RCC_SYSCLKConfig(RCC_SYSCLKSource_HSI);
    while (RCC_GetSYSCLKSource() != 0x00u) {
    }
    SystemCoreClockUpdate();

    /* 第二步: 再试着切到外部 8MHz 晶振 x9 = 72MHz。
     * 晶振起不来或者 PLL 没锁上, 就用 HSI 继续跑, 绝不卡死。 */
    (void)clock_try_hse72();
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
// 上电初始化: 72MHz 时钟 -> 1ms 时基 -> INTB 中断。
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
