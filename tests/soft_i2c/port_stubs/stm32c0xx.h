#ifndef TEST_C011_PORT_REGISTERS_H
#define TEST_C011_PORT_REGISTERS_H
#include <stdint.h>
typedef struct {
    volatile uint32_t APBENR2, APBRSTR2, IOPENR;
} RCC_TypeDef;
typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, BSRR;
} GPIO_TypeDef;
typedef struct {
    volatile uint32_t PSC, ARR, EGR, CNT, SR, CR1;
} TIM_TypeDef;
extern RCC_TypeDef port_test_rcc;
extern GPIO_TypeDef port_test_gpio_b;
extern TIM_TypeDef port_test_tim14;
extern uint32_t SystemCoreClock;
#define RCC (&port_test_rcc)
#define GPIOB (&port_test_gpio_b)
#define TIM14 (&port_test_tim14)
#define RCC_APBENR2_TIM14EN (1u << 15)
#define RCC_APBRSTR2_TIM14RST (1u << 15)
#define RCC_IOPENR_GPIOBEN (1u << 1)
#define TIM_EGR_UG 1u
#define TIM_CR1_CEN 1u
#endif
