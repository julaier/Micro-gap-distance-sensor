/*
 * adc_temp.c - 内部温度传感器实现
 *
 * 使用 ADC1 的内部通道 16 和 VREFINT 通道 17。
 * Vref 用 STM32 出厂校准值 (VREFINT_CAL @ 0x1FFFF7BA) 反算，
 * 温度公式按数据手册: V25 = 1.43V, 斜率 4.3mV/°C。
 *
 * 本文件使用 STM32F10x Standard Peripheral Library ADC/RCC API。
 */

#include "stm32f10x.h"
#include "adc_temp.h"
// VREFINT 校准值地址
#define VREFINT_CAL_ADDR   0x1FFFF7BAu

/* 调试用: 存最近一次温度采样的原始值 */
static uint16_t s_dbg_cal;
static uint16_t s_dbg_vref;
static uint16_t s_dbg_temp;

static uint16_t adc_convert(uint8_t channel)
{
    uint32_t guard;

    /* 单通道转换，每次切换通道都重新配置采样时间。 */
    ADC_RegularChannelConfig(ADC1, channel, 1u, ADC_SampleTime_239Cycles5);
    // 启动转换，等待结束
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    // 等转换完成: 计数等不到就返回, 不能卡死
    for (guard = 0u; guard < 100000u; guard++) {
        if (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) != RESET) {
            break;
        }
    }
    //返回 读取转换结果
    return ADC_GetConversionValue(ADC1);
}

void adc_temp_init(void)
{
    ADC_InitTypeDef adc;
    volatile uint32_t i;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);

    ADC_StructInit(&adc);
    adc.ADC_Mode                = ADC_Mode_Independent;
    adc.ADC_ScanConvMode        = DISABLE;
    adc.ADC_ContinuousConvMode  = DISABLE;
    adc.ADC_ExternalTrigConv    = ADC_ExternalTrigConv_None;
    adc.ADC_DataAlign           = ADC_DataAlign_Right;
    adc.ADC_NbrOfChannel        = 1u;
    ADC_Init(ADC1, &adc);

    ADC_Cmd(ADC1, ENABLE);

    /* 复位并重新校准 ADC。 */
    ADC_ResetCalibration(ADC1);
    for (i = 0u; i < 100000u; i++) {
        if (ADC_GetResetCalibrationStatus(ADC1) == RESET) {
            break;
        }
    }
    ADC_StartCalibration(ADC1);
    for (i = 0u; i < 100000u; i++) {
        if (ADC_GetCalibrationStatus(ADC1) == RESET) {
            break;
        }
    }

    /* 打开温度传感器与 VREFINT。 */
    ADC_TempSensorVrefintCmd(ENABLE);

    for (i = 0u; i < 20000u; i++) {   /* 等温度传感器稳定 */
    }

    (void)adc_convert(ADC_Channel_16);   /* 丢弃上电后第一组值 */
    (void)adc_convert(ADC_Channel_17);
}
// 读取温度值，返回值为摄氏度 * 10
int32_t adc_temp_read_x10(void)
{
    uint16_t raw_temp = adc_convert(ADC_Channel_16);
    uint16_t raw_vref = adc_convert(ADC_Channel_17);
    uint16_t cal = *(const uint16_t *)(uintptr_t)VREFINT_CAL_ADDR;
    float vdd;
    float vsense;
    float temp;

    s_dbg_cal  = cal;/*留给异常时的调试打印*/
    s_dbg_vref = raw_vref;
    s_dbg_temp = raw_temp;

    /* 出厂校准值没烧(读到 0 或 0xFFFF)时用经验值兜底 */
    if (cal == 0u || cal == 0xFFFFu) {
        cal = 1430u;
    }

    /* 先用 VREFINT 反推实际供电电压: VDD = 3.3V * 校准值 / 实测值。
     * VREFINT 通道要是读得离谱(接近 0), 就直接按 3.3V 算, 不然温度会飞。 */
    if (raw_vref < 100u) {
        vdd = 3.3f;
    } else {
        vdd = (3.3f * (float)cal) / (float)raw_vref;
    }

    // 温度传感器输出电压 = VDD * (ADC_TEMP / 4096)
    vsense = (vdd * (float)raw_temp) / 4096.0f;
    // 温度公式: T = (V25 - Vsense) / Avg_Slope + 25
    temp = (1.43f - vsense) / 0.0043f + 25.0f;

    /* 限幅: 异常值不要传到上位机 */
    if (temp < -40.0f) {
        temp = -40.0f;
    } else if (temp > 125.0f) {
        temp = 125.0f;
    }

    return (int32_t)(temp * 10.0f);
}
/* 调试用: 取最近一次采样里的原始值(校准值 / VREFINT / 温度通道) */
void adc_temp_debug(uint16_t *cal, uint16_t *raw_vref, uint16_t *raw_temp)
{
    if (cal != 0) {
        *cal = s_dbg_cal;
    }
    if (raw_vref != 0) {
        *raw_vref = s_dbg_vref;
    }
    if (raw_temp != 0) {
        *raw_temp = s_dbg_temp;
    }
}
