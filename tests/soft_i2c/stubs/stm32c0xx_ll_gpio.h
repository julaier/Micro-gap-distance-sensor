#ifndef TEST_LL_GPIO_H
#define TEST_LL_GPIO_H
#include "stm32c0xx.h"
#define LL_GPIO_PIN_6 (1u << 6)
#define LL_GPIO_PIN_7 (1u << 7)
#define LL_GPIO_MODE_INPUT 0u
#define LL_GPIO_MODE_OUTPUT 1u
#define LL_GPIO_SPEED_FREQ_HIGH 3u
#define LL_GPIO_OUTPUT_OPENDRAIN 1u
#define LL_GPIO_PULL_UP 1u
#define LL_GPIO_PULL_DOWN 2u
typedef struct {
    uint32_t Pin, Mode, Speed, OutputType, Pull, Alternate;
} LL_GPIO_InitTypeDef;
void LL_GPIO_SetOutputPin(GPIO_TypeDef *port, uint32_t pins);
void LL_GPIO_ResetOutputPin(GPIO_TypeDef *port, uint32_t pins);
uint32_t LL_GPIO_IsInputPinSet(const GPIO_TypeDef *port, uint32_t pins);
uint32_t LL_GPIO_ReadInputPort(const GPIO_TypeDef *port);
void LL_GPIO_Init(GPIO_TypeDef *port, LL_GPIO_InitTypeDef *config);
void LL_GPIO_SetPinMode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode);
#endif
