/*
 * adc_temp.h - STM32F103 内部温度传感器
 *
 * 用 ADC1 的内部通道 16 (温度) 与通道 17 (VREFINT) 估算芯片温度。
 * 注意: 这是 MCU 自身的温度, 不是探头温度, 只能做临时温补。
 * 无需占用任何外部引脚。
 */
#ifndef ADC_TEMP_H
#define ADC_TEMP_H

#include <stdint.h>

void adc_temp_init(void);

/* 返回摄氏度 x10, 例如 285 表示 28.5 摄氏度 */
int32_t adc_temp_read_x10(void);

/* 调试用: 取最近一次采样里的原始值, 温度异常时打出来查问题 */
void adc_temp_debug(uint16_t *cal, uint16_t *raw_vref, uint16_t *raw_temp);

#endif /* ADC_TEMP_H */
