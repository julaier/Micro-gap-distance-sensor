#ifndef TEST_STM32C0XX_H
#define TEST_STM32C0XX_H
#include <stdint.h>
typedef struct { uint32_t MODER, OTYPER; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpio_b;
#define GPIOB (&test_gpio_b)
typedef struct { uint32_t counter, prescaler, reload, enabled; } TIM_TypeDef;
extern TIM_TypeDef test_tim14;
extern uint32_t SystemCoreClock;
#define TIM14 (&test_tim14)
extern uint32_t test_irq_mask;
extern unsigned test_irq_disable_count, test_irq_restore_count;
static inline uint32_t __get_PRIMASK(void) { return test_irq_mask; }
static inline void __disable_irq(void)
{
    test_irq_mask = 1u;
    test_irq_disable_count++;
}
static inline void __set_PRIMASK(uint32_t value)
{
    test_irq_mask = value;
    test_irq_restore_count++;
}
#endif
