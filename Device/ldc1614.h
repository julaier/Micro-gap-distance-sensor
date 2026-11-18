/*
 * ldc1614.h - LDC1614RGHR 驱动 (电气接口层)
 *
 * 本板接法:
 *   SCL = PB6, SDA = PB7 (4.7K 上拉, 见 hard_i2c.h)
 *   ADDR = GND        -> 7 位地址 0x2A
 *   SD   = GND        -> 正常工作 (SD 高才是 shutdown)
 *   CLKIN <- 40MHz 有源晶振
 *   INTB = PB0        -> 推挽输出, 无需上拉, 接 EXTI0
 *   CH0 / CH1         -> 板载两路 PCB 平面线圈 (测量 / 参考)
 *   IN2 / IN3         -> 并接 GND, 不参与扫描
 */
#ifndef LDC1614_H
#define LDC1614_H

#include <stdint.h>

#define LDC1614_I2C_ADDR   0x2Au

/* 寄存器地址 */
#define LDC1614_REG_DATA_CH0_MSB        0x00u
#define LDC1614_REG_DATA_CH0_LSB        0x01u

#define LDC1614_REG_DATA_CH1_MSB        0x02u
#define LDC1614_REG_DATA_CH1_LSB        0x03u
//这一部分用不到
#define LDC1614_REG_DATA_CH2_MSB        0x04u
#define LDC1614_REG_DATA_CH2_LSB        0x05u
#define LDC1614_REG_DATA_CH3_MSB        0x06u
#define LDC1614_REG_DATA_CH3_LSB        0x07u

#define LDC1614_REG_RCOUNT_CH0          0x08u// 转换时间寄存器
#define LDC1614_REG_RCOUNT_CH1          0x09u// 转换时间寄存器
//无用
#define LDC1614_REG_RCOUNT_CH2          0x0Au
#define LDC1614_REG_RCOUNT_CH3          0x0Bu

#define LDC1614_REG_OFFSET_CH0          0x0Cu
#define LDC1614_REG_OFFSET_CH1          0x0Du
//无用
#define LDC1614_REG_OFFSET_CH2          0x0Eu
#define LDC1614_REG_OFFSET_CH3          0x0Fu

#define LDC1614_REG_SETTLECOUNT_CH0     0x10u// 建立时间寄存器
#define LDC1614_REG_SETTLECOUNT_CH1     0x11u// 建立时间寄存器
//无用
#define LDC1614_REG_SETTLECOUNT_CH2     0x12u
#define LDC1614_REG_SETTLECOUNT_CH3     0x13u

#define LDC1614_REG_CLOCK_DIVIDERS_CH0  0x14u/* 时钟分频寄存器 */
#define LDC1614_REG_CLOCK_DIVIDERS_CH1  0x15u/*时钟分频寄存器 */
//无用
#define LDC1614_REG_CLOCK_DIVIDERS_CH2  0x16u
#define LDC1614_REG_CLOCK_DIVIDERS_CH3  0x17u

#define LDC1614_REG_STATUS              0x18u
#define LDC1614_REG_ERROR_CONFIG        0x19u
#define LDC1614_REG_CONFIG              0x1Au/*用于配置外部参考时钟*/
#define LDC1614_REG_MUX_CONFIG          0x1Bu/* MUX 配置寄存器 ,MUX用于选择输入通道 */
#define LDC1614_REG_RESET_DEV           0x1Cu/* 软复位寄存器, 写 0x8000 才有效 */
#define LDC1614_REG_DRIVE_CURRENT_CH0   0x1Eu/* 驱动电流寄存器 */
#define LDC1614_REG_DRIVE_CURRENT_CH1   0x1Fu/* 驱动电流寄存器 */
//无用
#define LDC1614_REG_DRIVE_CURRENT_CH2   0x20u
#define LDC1614_REG_DRIVE_CURRENT_CH3   0x21u

#define LDC1614_REG_MANUFACTURER_ID     0x7Eu/* 厂商 ID 寄存器 */
#define LDC1614_REG_DEVICE_ID           0x7Fu/* 设备 ID 寄存器 */

/* DATA_CHx_MSB 的高 4 位是状态/错误标志 */
#define LDC1614_DATA_MASK        0x0FFFu /* 读取12 位数据用*/
#define LDC1614_ERR_UR           0x8000u   /* 转换未完成 */
#define LDC1614_ERR_OR           0x4000u   /* 溢出 */
#define LDC1614_ERR_WD           0x2000u   /* 看门狗 */
#define LDC1614_ERR_AE           0x1000u   /* 振幅错误 */
#define LDC1614_ERR_MASK         0xF000u  /* 读错误标志用 */
//硬件i2c初始化
void ldc1614_init_driver(void);

/* 调试用: 返回 I2C 空闲电平, bit0/1 = 内部下拉时 SCL/SDA, bit2/3 = 内部上拉时 SCL/SDA */
int  ldc1614_i2c_lines(void);

/* 调试用: 只发从机地址不发数据, 有 ACK 返回 0, 否则返回负数 */
int  ldc1614_probe(uint8_t addr7);

int  ldc1614_write_reg(uint8_t reg, uint16_t value);
int  ldc1614_read_reg(uint8_t reg, uint16_t *value);

/* 完整初始化, 成功返回 0; 失败返回负数 (见 .c 里的 ERR_xxx) */
int  ldc1614_init(uint16_t *device_id, uint16_t *manufacturer_id);

/* 读一次双通道转换结果; 同时取回两通道的错误标志 (可为 NULL) */
int  ldc1614_read_raw(uint32_t *ch0, uint32_t *ch1, uint16_t *err_flags);

int  ldc1614_read_status(uint16_t *status);

/* 把寄存器表回读到 out (每字节一个字节的寄存器值), 用于 DUMP 命令 */
int  ldc1614_dump_regs(uint8_t *out, uint8_t count);

#endif /* LDC1614_H */
