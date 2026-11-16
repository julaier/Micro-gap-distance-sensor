#ifndef MARGIN_STUB_STM32_H
#define MARGIN_STUB_STM32_H
#include <stdint.h>
typedef struct { volatile uint32_t IDR, BSRR; } margin_gpio_stub_t;
typedef struct { volatile uint32_t CNT; } margin_timer_stub_t;
extern margin_gpio_stub_t margin_gpio_b;
extern margin_timer_stub_t margin_timer14;
extern uint32_t margin_stub_mask;
#define GPIOB (&margin_gpio_b)
#define TIM14 (&margin_timer14)
static inline uint32_t __get_PRIMASK(void) { return margin_stub_mask; }
static inline void __disable_irq(void) { margin_stub_mask = 1u; }
static inline void __set_PRIMASK(uint32_t v) { margin_stub_mask = v; }
#endif
