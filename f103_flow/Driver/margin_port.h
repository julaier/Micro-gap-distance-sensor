#ifndef MARGIN_PORT_H
#define MARGIN_PORT_H
#include <stdint.h>

/* The same driver is tested against an edge-decoding slave. Only these
 * register operations are substituted; timing/protocol code is NOT mocked. */
#ifdef MARGIN_HOST
uint16_t margin_now(void);
uint8_t margin_pins(void);
void margin_scl(int release);
void margin_sda(int release);
uint32_t margin_lock(void);
void margin_unlock(uint32_t prior);
#else
#include "stm32c0xx.h"
static inline uint16_t margin_now(void) { return (uint16_t)TIM14->CNT; }
static inline uint8_t margin_pins(void)
{
    uint32_t pins = GPIOB->IDR;
    return (uint8_t)(((pins >> 7) & 1u) | (((pins >> 6) & 1u) << 1));
}
static inline void margin_scl(int release)
{
    GPIOB->BSRR = release ? (1u << 7) : (1u << 23);
}
static inline void margin_sda(int release)
{
    GPIOB->BSRR = release ? (1u << 6) : (1u << 22);
}
static inline uint32_t margin_lock(void)
{
    uint32_t prior = __get_PRIMASK();
    __disable_irq();
    return prior;
}
static inline void margin_unlock(uint32_t prior) { __set_PRIMASK(prior); }
#endif
#endif
