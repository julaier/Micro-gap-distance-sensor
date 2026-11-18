/*
 * stm32f10x_conf.h - STM32F10x 标准外设库模块配置
 *
 * Keil 工程已定义 USE_STDPERIPH_DRIVER，stm32f10x.h 会包含本文件。
 * 只打开本工程实际使用的 SPL 模块，避免无用的编译单元。
 */
#ifndef STM32F10X_CONF_H
#define STM32F10X_CONF_H

#include "stm32f10x_adc.h"
#include "stm32f10x_exti.h"
#include "stm32f10x_flash.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_i2c.h"
#include "stm32f10x_iwdg.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"
#include "misc.h"

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line);
#define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
#define assert_param(expr) ((void)0)
#endif /* USE_FULL_ASSERT */

#endif /* STM32F10X_CONF_H */
