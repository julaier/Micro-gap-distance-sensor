/*
 * ldc1614.c - LDC1614 驱动实现
 *
 * 初始化顺序按数据手册的建议:
 *   软复位 -> 时钟分频 -> 建立时间 -> 转换时间 -> 驱动电流
 *   -> MUX 扫描配置 -> CONFIG (最后写, 写完开始工作)
 * SETTLECOUNT 必须写在 RCOUNT 之前。
 *
 * 注意: 下面带 [核对] 标记的常量必须对照数据手册确认。
 *      上电时会回读校验, 不符会通过串口报错;
 *      也可以用调试命令 RD / WR / DUMP 在台架上直接改寄存器。
 */

#include "ldc1614.h"
#include "hard_i2c.h"
#include "board.h"

/* ------------------------------------------------------------------ *
 * 寄存器配置常量
 * ------------------------------------------------------------------ */

/* 转换时间: 0xFFFF -> 约 26.2ms/通道 @40MHz 参考时钟。
 * 采样率以后按实测再调, 只改这一个宏。 */
#define LDC1614_RCOUNT_VALUE        0xFFFFu

/* 建立时间: 需满足 SETTLECOUNT >= (fREF / fSENSOR) x Q。
 * 40MHz / 2.8MHz x 25 约= 357, 先取 384。 [核对] */
#define LDC1614_SETTLECOUNT_VALUE   0x0180u

/* 时钟分频: 复位默认值 0x1001。 [核对]
 * (FIN_DIV / FREF_DIV 的位域请按数据手册确认) */
#define LDC1614_CLOCK_DIV_DEFAULT   0x1001u

/* 器件 ID (SNOSCY9A 寄存器表) */
#define LDC1614_MANUFACTURER_ID     0x5449u
#define LDC1614_DEVICE_ID_LDC1614   0x3055u

/* 驱动电流初值: 自动幅度校正打开时, 这里给足够大的初值让它收敛。 [核对] */
#define LDC1614_DRIVE_CURRENT_VALUE 0x9000u

/* MUX_CONFIG: 打开自动扫描, 扫描序列 = CH0 + CH1。 [重点核对]
 * bit15 = AUTOSCAN_EN, bit14:13 = RR_SEQUENCE, 其余保留复位默认。 */
#define LDC1614_MUX_AUTOSCAN_CH01   0x820Cu/* = 复位默认 0x020C 再加 bit15 打开自动扫描 */

/* CONFIG: bit9 = REF_CLK_SRC, bit13 = SLEEP_MODE_EN。 */
#define LDC1614_CONFIG_SLEEP_MODE   0x2000u/* 休眠模式, 采样间隔长时可打开节能 */
#define LDC1614_CONFIG_REF_CLK_SRC  0x0200u/* 参考时钟选择: 置1=外部CLKIN, 清0=芯片内部时钟 */

#define LDC1614_RESET_MAGIC         0x8000u/* 写这个值到 RESET_DEV 才会软复位 */

/* 初始化出错时的返回码 */
#define LDC_ERR_RESET      (-10)/* 软复位失败 */
#define LDC_ERR_READ_ID    (-11)/* 读取 ID 失败 */
#define LDC_ERR_CLKDIV     (-12)/* 时钟分频设置失败 */
#define LDC_ERR_SETTLE     (-13)/* 建立时间设置失败 */
#define LDC_ERR_RCOUNT     (-14)/* 转换时间设置失败 */
#define LDC_ERR_DRIVE      (-15)/* 驱动电流设置失败 */
#define LDC_ERR_MUX        (-16)/*MUX通道扫描设置失败*/
#define LDC_ERR_CONFIG     (-17)/*CONFIG设置失败*/
#define LDC_ERR_VERIFY     (-18)/*校验失败*/

/* ------------------------------------------------------------------ */

void ldc1614_init_driver(void)
{
    hard_i2c_init();
}
//调试用: 返回 I2C 空闲电平, bit0/1 = 内部下拉时 SCL/SDA, bit2/3 = 内部上拉时 SCL/SDA
int ldc1614_i2c_lines(void)
{
    return (int)hard_i2c_lines();
}
//调试用: 只发从机地址, 从机应答返回 0, 不应答返回负数
int ldc1614_probe(uint8_t addr7)
{
    return hard_i2c_write(addr7, 0, 0u);
}
//写，成功了返回 0，失败返回负数，-1表示总线错误，-2表示从机 NACK，-3表示参数错误
int ldc1614_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t tx[3];

    tx[0] = reg;
    tx[1] = (uint8_t)(value >> 8);
    tx[2] = (uint8_t)(value & 0xFFu);

    return hard_i2c_write(LDC1614_I2C_ADDR, tx, 3u);
}
//读寄存器, 成功返回 0, 失败返回负数, -1表示总线错误, -2表示从机 NACK, -3表示参数错误
int ldc1614_read_reg(uint8_t reg, uint16_t *value)
{
    uint8_t rx[2];
    int rc;

    rc = hard_i2c_write_read(LDC1614_I2C_ADDR, &reg, 1u, rx, 2u);
    if (rc != 0) {
        return rc;
    }
    if (value != 0) {
        *value = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);
    }
    return 0;
}
//读状态寄存器, 成功返回 0, 失败返回负数, -1表示总线错误, -2表示从机 NACK, -3表示参数错误
int ldc1614_read_status(uint16_t *status)
{
    return ldc1614_read_reg(LDC1614_REG_STATUS, status);
}

/* 把寄存器 0x00 ~ (count-1) 读出来, 每字节放一个寄存器的低 8 位。
 * 只用于调试打印, 不参与测量流程。 */
//out: 读出的寄存器低 8 位, count 个字节,out用来接收数据
int ldc1614_dump_regs(uint8_t *out, uint8_t count)
{
    uint8_t i;

    if (out == 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        uint16_t v = 0u;
        if (ldc1614_read_reg(i, &v) != 0) {
            //读取失败, 返回 0xFF 表示错误
            out[i] = 0xFFu;
        } else {
            //读取成功，返回寄存器低 8 位
            out[i] = (uint8_t)(v & 0xFFu);
        }
    }
    return 0;
}

/* 读一个通道: MSB 寄存器低 12 位 + LSB 寄存器 16 位 = 28 位数据,
 * 同时把 MSB 的高 4 位错误标志传出去。 */
static int read_channel(uint8_t reg_msb, uint8_t reg_lsb,
                        uint32_t *value, uint16_t *err)
{
    uint16_t msb;
    uint16_t lsb;

    if (ldc1614_read_reg(reg_msb, &msb) != 0) {
        return -1;
    }
    if (ldc1614_read_reg(reg_lsb, &lsb) != 0) {
        return -1;
    }

    *value = (((uint32_t)(msb & LDC1614_DATA_MASK)) << 16) | (uint32_t)lsb;
    if (err != 0) {
        *err = (uint16_t)(msb & LDC1614_ERR_MASK);
    }
    return 0;
}
// 读一次双通道转换结果; 同时取回两通道的错误标志 (可为 NULL),返回0表示成功, 负数表示失败
int ldc1614_read_raw(uint32_t *ch0, uint32_t *ch1, uint16_t *err_flags)
{
    uint32_t a = 0u;
    uint32_t b = 0u;
    uint16_t ea = 0u;
    uint16_t eb = 0u;

    if (read_channel(LDC1614_REG_DATA_CH0_MSB, LDC1614_REG_DATA_CH0_LSB, &a, &ea) != 0) {
        return -1;
    }
    if (read_channel(LDC1614_REG_DATA_CH1_MSB, LDC1614_REG_DATA_CH1_LSB, &b, &eb) != 0) {
        return -1;
    }

    if (ch0 != 0) {
        *ch0 = a;
    }
    if (ch1 != 0) {
        *ch1 = b;
    }
    if (err_flags != 0) {
        *err_flags = (uint16_t)(ea | eb);
    }
    return 0;
}
// 完整初始化, 成功返回 0; 失败返回负数 (见 .c 里的 ERR_xxx)
// device_id用于接收器件 ID, manufacturer_id用于接收厂商 ID, 都可以为 NULL
int ldc1614_init(uint16_t *device_id, uint16_t *manufacturer_id)
{
    uint16_t v = 0u;
    uint16_t mid = 0u;
    uint16_t did = 0u;

    /* 1. 软复位 */
    if (ldc1614_write_reg(LDC1614_REG_RESET_DEV, LDC1614_RESET_MAGIC) != 0) {
        return LDC_ERR_RESET;
    }
    board_delay_ms(5u);

    /* 2. 读并校验 ID, 防止把其它器件误当成 LDC1614 */
    if (ldc1614_read_reg(LDC1614_REG_MANUFACTURER_ID, &mid) != 0 ||
        ldc1614_read_reg(LDC1614_REG_DEVICE_ID, &did) != 0) {
        return LDC_ERR_READ_ID;
    }
    //  把 ID 返回给调用者, 并检查是否符合预期
    if (manufacturer_id != 0) {
        *manufacturer_id = mid;
    }
    if (device_id != 0) {
        *device_id = did;
    }
    //  核对 ID, 不符就报错, 以防误操作
    if (mid != LDC1614_MANUFACTURER_ID || did != LDC1614_DEVICE_ID_LDC1614) {
        return LDC_ERR_READ_ID;
    }

    /* 3. 通道时钟分频 (CH0 / CH1) */
    if (ldc1614_write_reg(LDC1614_REG_CLOCK_DIVIDERS_CH0, LDC1614_CLOCK_DIV_DEFAULT) != 0 ||
        ldc1614_write_reg(LDC1614_REG_CLOCK_DIVIDERS_CH1, LDC1614_CLOCK_DIV_DEFAULT) != 0) {
        return LDC_ERR_CLKDIV;
    }

    /* 4. 建立时间 (必须先于 RCOUNT) */
    if (ldc1614_write_reg(LDC1614_REG_SETTLECOUNT_CH0, LDC1614_SETTLECOUNT_VALUE) != 0 ||
        ldc1614_write_reg(LDC1614_REG_SETTLECOUNT_CH1, LDC1614_SETTLECOUNT_VALUE) != 0) {
        return LDC_ERR_SETTLE;
    }

    /* 5. 转换时间 */
    if (ldc1614_write_reg(LDC1614_REG_RCOUNT_CH0, LDC1614_RCOUNT_VALUE) != 0 ||
        ldc1614_write_reg(LDC1614_REG_RCOUNT_CH1, LDC1614_RCOUNT_VALUE) != 0) {
        return LDC_ERR_RCOUNT;
    }

    /* 6. 驱动电流初值 (交给自动幅度校正继续调) */
    if (ldc1614_write_reg(LDC1614_REG_DRIVE_CURRENT_CH0, LDC1614_DRIVE_CURRENT_VALUE) != 0 ||
        ldc1614_write_reg(LDC1614_REG_DRIVE_CURRENT_CH1, LDC1614_DRIVE_CURRENT_VALUE) != 0) {
        return LDC_ERR_DRIVE;
    }

    /* 7. MUX: 自动扫描 CH0 + CH1 */
    if (ldc1614_write_reg(LDC1614_REG_MUX_CONFIG, LDC1614_MUX_AUTOSCAN_CH01) != 0) {
        return LDC_ERR_MUX;
    }

    /* 8. CONFIG: 用外部 40MHz 有源晶振做参考时钟, 并退出复位后的 Sleep Mode。 */
    if (ldc1614_read_reg(LDC1614_REG_CONFIG, &v) != 0) {
        return LDC_ERR_CONFIG;
    }
    // 清掉 SLEEP_MODE, 再把 REF_CLK_SRC 置 1: 用外部 CLKIN(40MHz), 最后写回寄存器
    v &= (uint16_t)~(LDC1614_CONFIG_SLEEP_MODE | LDC1614_CONFIG_REF_CLK_SRC);
    v |= LDC1614_CONFIG_REF_CLK_SRC;
    if (ldc1614_write_reg(LDC1614_REG_CONFIG, v) != 0) {
        return LDC_ERR_CONFIG;
    }

    /* 9. 回读校验, 配错时上电就能发现 */
    if (ldc1614_read_reg(LDC1614_REG_CONFIG, &v) != 0) {
        return LDC_ERR_VERIFY;
    }
    // 校验 CONFIG 寄存器, 确认 REF_CLK_SRC=1(外部时钟), SLEEP_MODE 位被清除
    if ((v & LDC1614_CONFIG_REF_CLK_SRC) == 0u ||
        (v & LDC1614_CONFIG_SLEEP_MODE) != 0u) {
        return LDC_ERR_VERIFY;
    }

    return 0;
}
