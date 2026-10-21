/*
 * hard_i2c.h - STM32F103 硬件 I2C1 主机接口
 *
 * SCL = PB6, SDA = PB7, 复用开漏，板级 4.7K 上拉。
 * LDC1614 支持最高 400kHz I2C。
 */
#ifndef HARD_I2C_H
#define HARD_I2C_H

#include <stdint.h>
/* 初始化 I2C 外设，400kHz，复用开漏。 */
void hard_i2c_init(void);

/* 调试用: bit0/1 = 内部下拉时 SCL/SDA 电平, bit2/3 = 内部上拉时 SCL/SDA 电平。 */
uint8_t hard_i2c_lines(void);

/* 返回 0 成功；负数表示 NACK、超时或参数错误。 */
int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len);

/* 寄存器读时序: 写寄存器地址 -> 重复起始 -> 读 1 或 2 字节。 */
int hard_i2c_write_read(uint8_t addr7,
                        const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen);

#endif /* HARD_I2C_H */
