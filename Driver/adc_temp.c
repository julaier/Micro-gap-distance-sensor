/*
 * adc_temp.c - STM32C011 内部温度传感器
 *
 * ADC1 内部温度通道 + VREFINT。温度结果使用 C0 芯片内 TS_CAL1
 * 和 VREFINT_CAL 校准值估算，输出单位为 0.1 摄氏度。
 */

#include "stm32c0xx.h"
#include "stm32c0xx_ll_adc.h"
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_rcc.h"
#include "adc_temp.h"

#define ADC_TIMEOUT_GUARD   200000u

static uint16_t s_dbg_cal;
static uint16_t s_dbg_vref;
static uint16_t s_dbg_temp;

static int adc_select_channel(uint32_t channel)
{
    uint32_t guard;

    LL_ADC_REG_SetSequencerChAdd(ADC1, channel);
    for (guard = 0u; guard < ADC_TIMEOUT_GUARD; guard++) {
        if (LL_ADC_IsActiveFlag_CCRDY(ADC1) != 0u) {
            LL_ADC_ClearFlag_CCRDY(ADC1);
            return 0;
        }
    }
    return -1;
}

static int adc_convert(uint32_t channel, uint16_t *value)
{
    uint32_t guard;

    if (adc_select_channel(channel) != 0) {
        return -1;
    }

    LL_ADC_REG_StartConversion(ADC1);
    for (guard = 0u; guard < ADC_TIMEOUT_GUARD; guard++) {
        if (LL_ADC_IsActiveFlag_EOC(ADC1) != 0u) {
            *value = (uint16_t)LL_ADC_REG_ReadConversionData32(ADC1);
            LL_ADC_ClearFlag_EOC(ADC1);
            return 0;
        }
    }
    return -1;
}

void adc_temp_init(void)
{
    LL_ADC_InitTypeDef adc = {0};
    LL_ADC_REG_InitTypeDef reg = {0};
    volatile uint32_t guard;

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_ADC);
    LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_SYSCLK);

    adc.Clock = LL_ADC_CLOCK_SYNC_PCLK_DIV2; /* 8 MHz -> 4 MHz */
    adc.Resolution = LL_ADC_RESOLUTION_12B;
    adc.DataAlignment = LL_ADC_DATA_ALIGN_RIGHT;
    adc.LowPowerMode = LL_ADC_LP_MODE_NONE;
    LL_ADC_Init(ADC1, &adc);

    LL_ADC_REG_SetSequencerConfigurable(ADC1, LL_ADC_REG_SEQ_FIXED);
    reg.TriggerSource = LL_ADC_REG_TRIG_SOFTWARE;
    reg.SequencerLength = LL_ADC_REG_SEQ_SCAN_DISABLE;
    reg.SequencerDiscont = LL_ADC_REG_SEQ_DISCONT_DISABLE;
    reg.ContinuousMode = LL_ADC_REG_CONV_SINGLE;
    reg.DMATransfer = LL_ADC_REG_DMA_TRANSFER_NONE;
    reg.Overrun = LL_ADC_REG_OVR_DATA_OVERWRITTEN;
    LL_ADC_REG_Init(ADC1, &reg);
    LL_ADC_REG_SetSequencerScanDirection(ADC1, LL_ADC_REG_SEQ_SCAN_DIR_FORWARD);
    LL_ADC_SetOverSamplingScope(ADC1, LL_ADC_OVS_DISABLE);
    LL_ADC_SetTriggerFrequencyMode(ADC1, LL_ADC_CLOCK_FREQ_MODE_HIGH);
    LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_TEMPSENSOR,
                                  LL_ADC_SAMPLINGTIME_COMMON_1);
    LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_VREFINT,
                                  LL_ADC_SAMPLINGTIME_COMMON_1);
    LL_ADC_SetSamplingTimeCommonChannels(ADC1, LL_ADC_SAMPLINGTIME_COMMON_1,
                                         LL_ADC_SAMPLINGTIME_79CYCLES_5);
    LL_ADC_DisableIT_EOC(ADC1);
    LL_ADC_DisableIT_EOS(ADC1);

    LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(ADC1),
                                   LL_ADC_PATH_INTERNAL_VREFINT |
                                   LL_ADC_PATH_INTERNAL_TEMPSENSOR);
    (void)adc_select_channel(LL_ADC_CHANNEL_TEMPSENSOR);

    LL_ADC_EnableInternalRegulator(ADC1);
    for (guard = 0u; guard < 10000u; guard++) {
    }

    LL_ADC_StartCalibration(ADC1);
    for (guard = 0u; guard < ADC_TIMEOUT_GUARD; guard++) {
        if (LL_ADC_IsCalibrationOnGoing(ADC1) == 0u) {
            break;
        }
    }

    LL_ADC_Enable(ADC1);
    for (guard = 0u; guard < ADC_TIMEOUT_GUARD; guard++) {
        if (LL_ADC_IsActiveFlag_ADRDY(ADC1) != 0u) {
            break;
        }
    }

    {
        uint16_t dummy;
        (void)adc_convert(LL_ADC_CHANNEL_TEMPSENSOR, &dummy);
        (void)adc_convert(LL_ADC_CHANNEL_VREFINT, &dummy);
    }
}

int32_t adc_temp_read_x10(void)
{
    uint16_t raw_temp = 0u;
    uint16_t raw_vref = 0u;
    uint16_t cal = *VREFINT_CAL_ADDR;
    uint16_t ts_cal = *TEMPSENSOR_CAL1_ADDR;
    uint32_t normalized_temp;
    int32_t temp;

    if (adc_convert(LL_ADC_CHANNEL_TEMPSENSOR, &raw_temp) != 0 ||
        adc_convert(LL_ADC_CHANNEL_VREFINT, &raw_vref) != 0 ||
        raw_vref < 100u) {
        return 250;
    }

    s_dbg_cal = cal;
    s_dbg_vref = raw_vref;
    s_dbg_temp = raw_temp;

    normalized_temp = ((uint32_t)raw_temp * (uint32_t)cal) / raw_vref;
    temp = 30 + (((int32_t)normalized_temp - (int32_t)ts_cal) * 1000) / 4300;

    if (temp < -40) {
        temp = -40;
    } else if (temp > 125) {
        temp = 125;
    }
    return temp * 10;
}

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
