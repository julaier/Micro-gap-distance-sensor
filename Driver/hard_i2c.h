/*
 * hard_i2c.h - STM32C011 软件 I2C 主机接口（保留原接口名）
 *
 * SCL = PB7, SDA = PB6, GPIO 开漏，板上 4.7K 上拉。
 */
#ifndef HARD_I2C_H
#define HARD_I2C_H

#include <stdint.h>

/* V9 固定用户指定引脚，不再支持旧 V4 接线矩阵。 */
#ifndef HARD_I2C_DIAGNOSTIC_MATRIX
#define HARD_I2C_DIAGNOSTIC_MATRIX 0
#endif

typedef enum {
    HARD_I2C_IDLE = 0,
    HARD_I2C_START,
    HARD_I2C_ADDR_W,
    HARD_I2C_WRITE,
    HARD_I2C_RESTART,
    HARD_I2C_ADDR_R,
    HARD_I2C_READ,
    HARD_I2C_STOP,
    HARD_I2C_DONE
} hard_i2c_stage_t;

typedef struct {
    int rc;                  /* 首次失败原因；恢复总线前保存 */
    hard_i2c_stage_t stage;
    uint16_t byte_index;     /* 数据字节下标，从 0 开始 */
    uint8_t lines;           /* bit0=SCL(PB7)，bit1=SDA(PB6) */
    uint16_t period_min_us;  /* V6/V7 兼容保留，V9 不采集，恒为 0 */
    uint16_t period_max_us;  /* V9 不采集，恒为 0 */
    uint16_t period_min_ticks; /* V9 不采集最小/最大周期，恒为 0 */
    uint16_t period_max_ticks;
    uint16_t period_min_us_x10; /* V9 不采集，恒为 0 */
    uint16_t period_max_us_x10;
    uint32_t period_total_ticks; /* V9 原 test 不导出此字段，恒为 0 */
    uint32_t effective_hz;   /* 最近完成发送的字节，8 周期本机估算 Hz */
    uint16_t half_ticks;     /* V9 半拍最小等待 5，1 tick=1us；不含软件开销 */
    uint8_t clocks;          /* 完整发送一个字节及 ACK 时为 9 */
    uint8_t tx_byte;         /* 最近发送的字节，非 7 位地址 */
    uint8_t sampled_byte;    /* 旧字段保留；V9 不逐位回采，恒为 255，不再输出 */
    uint8_t ack_samples;     /* 旧字段保留；V9 不三次采样，恒为 255，不再输出 */
    uint8_t ack_sda_mode;    /* V9 不额外采集，恒为 255；原 test 显式输入 */
    uint8_t ack_sda_od;      /* V9 不额外采集，恒为 255 */
    uint8_t ack_level;       /* 第九拍实际采样：0=ACK，1=NACK，255=未采样 */
    int stop_rc;             /* STOP 结果单独报告，不覆盖首次失败 */
} hard_i2c_diag_t;

void hard_i2c_init(void);
void hard_i2c_get_diag(hard_i2c_diag_t *diag);
/* 旧 API 保留；V9 不调频，返回 -3，且不产生总线事务。 */
int hard_i2c_calibrate_100k(uint8_t addr7);

/* 旧 API 保留：参数不再改变接线和接收模式，仅 STOP 释放总线。
 * 固定 PB7=SCL/PB6=SDA，ACK 和读数据时始终显式输入。 */
void hard_i2c_debug_config(uint8_t swapped, uint8_t input_rx);

/* 输出四次实际电平: 空闲、拉低 SCL、拉低 SDA、释放；正常为 3/2/1/3。
 * 只在传输结束后调用。不能据此确认 MCU 与 LDC 之间的导线是否连通。 */
void hard_i2c_pin_test(uint8_t levels[4]);

/* 旧下拉/上拉测试 API 保留，但 V9 不操作引脚，返回 255 表示未采集。 */
uint8_t hard_i2c_lines(void);

/* 返回: 0 成功，-1 总线异常/超时，-2 从机 NACK，-3 参数错误。 */
int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len);

int hard_i2c_write_read(uint8_t addr7,
                        const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen);

#endif /* HARD_I2C_H */
