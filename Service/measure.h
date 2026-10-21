/*
 * measure.h - 测量算法层
 *
 * 数据流: CH0/CH1 原始值 -> 比值 -> 中值滤波 -> 均值滤波 -> 温补
 *         -> 标定表分段插值 -> 迟滞 + 去抖 -> 状态
 *
 * 本层不访问任何硬件寄存器, 可以单独在 PC 上编译测试。
 */
#ifndef MEASURE_H
#define MEASURE_H

#include <stdint.h>

#define MEAS_MAX_POINTS   9/* 标定点最大个数, 1~9 个 */

/* 状态码, 也是串口 $D / $R 帧的最后一个字段 */
#define MEAS_ST_NORMAL    0u   /* 正常 */
#define MEAS_ST_WARN      1u   /* 提醒 */
#define MEAS_ST_ALARM     2u   /* 报警 */
#define MEAS_ST_FAULT     3u   /* 故障 (I2C / INTB 异常) */
#define MEAS_ST_UNCAL     4u   /* 未标定, 距离无效 */

typedef struct {
    float    dist_mm;      /* 间隙, mm */
    float    ratio;        /* 温补后的比值 */
    float    temp_c;       /* 温度, 摄氏度 */
    uint32_t ch0;          /* 通道 0 原始 28 位值 */
    uint32_t ch1;          /* 通道 1 原始 28 位值 */
    uint8_t  status;       /* MEAS_ST_xxx */
    uint8_t  valid;        /* 1 = 距离有效 */
} measure_result_t;

void measure_init(void);
void measure_load(void);         /* 从 Flash 读回参数 */
void measure_save(void);         /* 参数写入 Flash */

const measure_result_t *measure_latest(void);/* 最近一次测量结果 */

void measure_set_thresholds(float alarm_mm, float warn_mm, float hyst_mm);/* 设置报警/提醒阈值和迟滞, mm */
void measure_get_thresholds(float *alarm_mm, float *warn_mm, float *hyst_mm);/* 获取报警/提醒阈值和迟滞, mm */

/* 标定点按 1,2,3... 顺序录入, 录的是当前滤波后的比值 */
int  measure_cal_set(int index, float gap_mm);/* 设置标定点 */
int  measure_cal_count(void);/* 获取标定点个数 */
int  measure_cal_get(int index, float *gap_mm, float *ratio);  /* index 从 0 开始 */
void measure_cal_clear(void);/* 清除所有标定点 */

/* 温补系数: ratio = ratio_raw - k * (T - Tref) */
void measure_set_temp_comp(float ratio_per_c, float ref_c);/* 设置温补系数, k = 比值/摄氏度, Tref = 参考温度 */
void measure_get_temp_comp(float *ratio_per_c, float *ref_c);/* 获取温补系数 */

/* CH0 / CH1 哪个是测量线圈暂时不确定, swap = 1 时用 CH1/CH0 */
void measure_set_swap(uint8_t swap);/* 设置通道交换 */
uint8_t measure_get_swap(void);/* 获取通道交换状态 */

void measure_fault(void);/* 进入故障状态, 距离无效 */
void measure_update(uint32_t ch0, uint32_t ch1, float temp_c, uint32_t now_ms);/* 更新测量结果, ch0/ch1 = 原始值, temp_c = 温度, now_ms = 当前时间戳 */

#endif /* MEASURE_H */
