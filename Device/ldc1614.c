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

/* 时钟分频配置 0x1001：FIN_DIVIDER=1，FREF_DIVIDER=1。
 * 这是本工程写入值，不是器件复位默认值。 */
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
#define LDC_ERR_CONFIG     LDC1614_ERR_CONFIG/*CONFIG设置失败*/
#define LDC_ERR_VERIFY     (-18)/*校验失败*/
#define LDC_ERR_INT_CONFIG (-19)/*数据就绪中断配置失败*/

static ldc1614_init_trace_t s_init_trace;
static hard_i2c_diag_t s_init_failure;
static uint8_t s_have_init_failure;

void ldc1614_set_init_trace(ldc1614_init_trace_t trace)
{
    s_init_trace = trace;
}

void ldc1614_get_init_bus_diag(hard_i2c_diag_t *diag)
{
    if (diag == 0) return;
    if (s_have_init_failure) *diag = s_init_failure;
    else hard_i2c_get_diag(diag);
}

/* Do not mistake a sampled ACK for successful configuration. Verify the value.
 * A CONFIG final-byte NACK is only reconcilable when a later full read proves
 * that the exact requested value was accepted. Never forgive an address/data
 * prefix NACK, bus timeout, failed STOP, failed read or mismatched value. */
static int init_write_verify(uint8_t reg, uint16_t value)
{
    ldc1614_init_step_t step = {0};
    int can_verify;

    step.reg = reg;
    step.expected = value;
    step.read_rc = -4;
    step.write_rc = ldc1614_write_reg(reg, value);
    hard_i2c_get_diag(&step.bus);
    can_verify = step.write_rc == 0 ||
        (reg == LDC1614_REG_CONFIG && step.write_rc == -2 &&
         step.bus.stage == HARD_I2C_WRITE && step.bus.byte_index == 2u &&
         step.bus.clocks == 9u && step.bus.ack_level == 1u &&
         step.bus.stop_rc == 0);

    step.result = step.write_rc;
    if (can_verify) {
        /* Keep clock selection and wake-up separate, then allow settling. */
        if (reg == LDC1614_REG_CONFIG) board_delay_ms(5u);
        step.read_rc = ldc1614_read_reg(reg, &step.actual);
        if (step.write_rc == 0 && step.read_rc != 0)
            hard_i2c_get_diag(&step.bus);
        if (step.read_rc == 0 && step.actual == value) {
            step.result = 0;
            step.nack_verified = step.write_rc == -2 ? 1u : 0u;
        } else if (step.write_rc == 0) {
            step.result = step.read_rc != 0 ? step.read_rc : LDC_ERR_VERIFY;
        }
    }
    if (step.result != 0) {
        s_init_failure = step.bus;
        s_have_init_failure = 1u;
    }
    if (s_init_trace != 0) s_init_trace(&step);
    return step.result;
}

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
/* Check both unread flags before consuming a pair. */
int ldc1614_read_fresh_raw(uint32_t *ch0, uint32_t *ch1,
                          uint16_t *err_flags, uint16_t *status)
{
    uint16_t st = 0u, errors = 0u;
    uint32_t a = 0u, b = 0u;
    int rc = ldc1614_read_status(&st);
    if (status != 0) *status = st;
    if (rc != 0) return rc;
    if ((st & LDC1614_STATUS_CH01_UNREAD) != LDC1614_STATUS_CH01_UNREAD)
        return 1;
    rc = ldc1614_read_raw(&a, &b, &errors);
    if (rc != 0) return rc;
    if (ch0 != 0) *ch0 = a;
    if (ch1 != 0) *ch1 = b;
    if (err_flags != 0) *err_flags = errors;
    return a != 0u && b != 0u && errors == 0u ? 0 : LDC1614_ERR_NO_DATA;
}

int ldc1614_init(uint16_t *device_id, uint16_t *manufacturer_id)
{
    return ldc1614_init_clock(device_id, manufacturer_id, 1);
}

int ldc1614_init_clock(uint16_t *device_id, uint16_t *manufacturer_id,
                      int external)
{
    uint16_t v = 0u;
    uint16_t mid = 0u;
    uint16_t did = 0u;

    s_have_init_failure = 0u;

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
    if (init_write_verify(LDC1614_REG_CLOCK_DIVIDERS_CH0, LDC1614_CLOCK_DIV_DEFAULT) != 0 ||
        init_write_verify(LDC1614_REG_CLOCK_DIVIDERS_CH1, LDC1614_CLOCK_DIV_DEFAULT) != 0) {
        return LDC_ERR_CLKDIV;
    }

    /* 4. 建立时间 (必须先于 RCOUNT) */
    if (init_write_verify(LDC1614_REG_SETTLECOUNT_CH0, LDC1614_SETTLECOUNT_VALUE) != 0 ||
        init_write_verify(LDC1614_REG_SETTLECOUNT_CH1, LDC1614_SETTLECOUNT_VALUE) != 0) {
        return LDC_ERR_SETTLE;
    }

    /* 5. 转换时间 */
    if (init_write_verify(LDC1614_REG_RCOUNT_CH0, LDC1614_RCOUNT_VALUE) != 0 ||
        init_write_verify(LDC1614_REG_RCOUNT_CH1, LDC1614_RCOUNT_VALUE) != 0) {
        return LDC_ERR_RCOUNT;
    }

    /* 6. 驱动电流初值 (交给自动幅度校正继续调) */
    if (init_write_verify(LDC1614_REG_DRIVE_CURRENT_CH0, LDC1614_DRIVE_CURRENT_VALUE) != 0 ||
        init_write_verify(LDC1614_REG_DRIVE_CURRENT_CH1, LDC1614_DRIVE_CURRENT_VALUE) != 0) {
        return LDC_ERR_DRIVE;
    }

    /* 7. MUX: 自动扫描 CH0 + CH1 */
    if (init_write_verify(LDC1614_REG_MUX_CONFIG, LDC1614_MUX_AUTOSCAN_CH01) != 0) {
        return LDC_ERR_MUX;
    }

    /* Enable DRDY -> INTB; reset leaves ERROR_CONFIG.DRDY_2INT disabled.
     * Keep the 20ms polling fallback in the application as well. */
    if (init_write_verify(LDC1614_REG_ERROR_CONFIG, 0x0001u) != 0) {
        return LDC_ERR_INT_CONFIG;
    }

    /* 8. Select reference while asleep, verify, then wake. Only bit9 differs
     * between attempts. Multi-channel internal reference permits FREF <=55MHz
     * (TI table43), so keep the proven /1 divider and all old sensor settings. */
    if (ldc1614_read_reg(LDC1614_REG_CONFIG, &v) != 0) {
        return LDC_ERR_CONFIG;
    }
    v |= LDC1614_CONFIG_SLEEP_MODE;
    v &= (uint16_t)~LDC1614_CONFIG_REF_CLK_SRC;
    if (external) v |= LDC1614_CONFIG_REF_CLK_SRC;
    if (init_write_verify(LDC1614_REG_CONFIG, v) != 0) {
        return LDC_ERR_CONFIG;
    }

    /* 9. Wake without changing clock source in the same transaction.
     * Exact target after reset: external=0x0A01, internal=0x0801. */
    v &= (uint16_t)~LDC1614_CONFIG_SLEEP_MODE;
    if (init_write_verify(LDC1614_REG_CONFIG, v) != 0) {
        return LDC_ERR_CONFIG;
    }

    return 0;
}
