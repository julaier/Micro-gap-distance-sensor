#ifndef TEST_LL_BUS_H
#define TEST_LL_BUS_H
#include "stm32c0xx.h"
#define LL_IOP_GRP1_PERIPH_GPIOB 2u
void LL_IOP_GRP1_EnableClock(uint32_t clock);
#define LL_APB2_GRP1_PERIPH_TIM14 (1u << 15)
void LL_APB2_GRP1_EnableClock(uint32_t clock);
void LL_APB2_GRP1_ForceReset(uint32_t clock);
void LL_APB2_GRP1_ReleaseReset(uint32_t clock);
#endif
