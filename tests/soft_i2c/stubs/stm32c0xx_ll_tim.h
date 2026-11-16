#ifndef TEST_LL_TIM_H
#define TEST_LL_TIM_H
#include <assert.h>
#include "stm32c0xx.h"
/* One tick is 0.25 us. Optional GPIO costs let tests inject execution
 * overhead independently of the driver's chosen half-period budget. */
extern uint32_t test_timer_ticks, test_timer_step;
static inline void test_advance_ticks(uint32_t count)
{
    test_tim14.counter = (test_tim14.counter + count) & test_tim14.reload;
    test_timer_ticks += count;
}
static inline void LL_TIM_SetPrescaler(TIM_TypeDef *timer, uint32_t value)
{
    timer->prescaler = value;
}
static inline void LL_TIM_SetAutoReload(TIM_TypeDef *timer, uint32_t value)
{
    timer->reload = value;
}
static inline void LL_TIM_GenerateEvent_UPDATE(TIM_TypeDef *timer)
{
    assert(timer == TIM14 && timer->prescaler + 1u == SystemCoreClock / 4000000u);
}
static inline void LL_TIM_SetCounter(TIM_TypeDef *timer, uint32_t value)
{
    timer->counter = value;
}
static inline void LL_TIM_EnableCounter(TIM_TypeDef *timer) { timer->enabled = 1; }
static inline uint32_t LL_TIM_GetCounter(const TIM_TypeDef *timer)
{
    uint32_t value = timer->counter;
    assert(timer == TIM14 && timer->enabled);
    test_advance_ticks(test_timer_step);
    return value;
}
#endif
