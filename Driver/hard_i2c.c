/*
 * hard_i2c.c - STM32F103 硬件 I2C1 主机实现
 *
 * SCL = PB6, SDA = PB7, 400kHz, 复用开漏。
 * 使用 STM32F10x Standard Peripheral Library 的阻塞式收发流程，
 * 仅访问本工程需要的 LDC1614 寄存器。
 */

#include "stm32f10x.h"
#include "hard_i2c.h"
#include "board.h"

#define I2C_TIMEOUT_MS   10u
#define I2C_SCL_PIN      GPIO_Pin_6
#define I2C_SDA_PIN      GPIO_Pin_7

/*
 * I2C1 总线错误标志检查，返回 0 表示无错误，-1 表示总线错误，-2 表示从机 NACK。
 */
static int i2c_error_code(void)
{
    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_AF) != RESET) {
        return -2;   /* 从机 NACK 或地址错误 */
    }
    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_ARLO) != RESET ||
        I2C_GetFlagStatus(I2C1, I2C_FLAG_BERR) != RESET ||
        I2C_GetFlagStatus(I2C1, I2C_FLAG_OVR) != RESET) {
        return -1;
    }
    return 0;
}
// 等待标志位置位，超时返回 -1，错误返回 -2。
static int wait_flag_set(uint32_t flag)
{
    uint32_t start = board_millis();

    while (I2C_GetFlagStatus(I2C1, flag) == RESET) {
        int err = i2c_error_code();
        if (err != 0) {
            return err;
        }
        if ((uint32_t)(board_millis() - start) > I2C_TIMEOUT_MS) {
            return -1;
        }
    }
    return 0;
}
// 等待标志位清除，超时返回 -1，错误返回 -2。
static int wait_flag_clear(uint32_t flag)
{
    uint32_t start = board_millis();
//看标志位flag为reset相当于没有发送
    while (I2C_GetFlagStatus(I2C1, flag) != RESET) {
        int err = i2c_error_code();
        if (err != 0) {
            return err;
        }
        if ((uint32_t)(board_millis() - start) > I2C_TIMEOUT_MS) {
            return -1;
        }
    }
    return 0;
}

static void clear_error_flags(void)
{
    I2C_ClearFlag(I2C1, I2C_FLAG_AF | I2C_FLAG_ARLO |
                        I2C_FLAG_BERR | I2C_FLAG_OVR);
}
//清理 ADDR 标志位，必须在读 SR1 后读 SR2。
static void clear_addr_flag(void)
{
    /* ADDR 的清除序列: 读 SR1，再读 SR2。 */
    (void)I2C_ReadRegister(I2C1, I2C_Register_SR1);
    (void)I2C_ReadRegister(I2C1, I2C_Register_SR2);
}
/* 复用开漏 GPIO 模式配置。 */
static void i2c_gpio_config(GPIOMode_TypeDef mode)
{
    GPIO_InitTypeDef gpio;

    gpio.GPIO_Pin   = I2C_SCL_PIN | I2C_SDA_PIN;
    gpio.GPIO_Mode  = mode;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);
}

static void i2c_bus_recover(void)
{
    uint8_t i;

    I2C_Cmd(I2C1, DISABLE);

    /* 临时改成开漏 GPIO，手动发 9 个时钟和一个 STOP。 */
    i2c_gpio_config(GPIO_Mode_Out_OD);
    GPIO_SetBits(GPIOB, I2C_SCL_PIN | I2C_SDA_PIN);

    for (i = 0u; i < 9u; i++) {
        GPIO_ResetBits(GPIOB, I2C_SCL_PIN);
        board_delay_us(2u);
        GPIO_SetBits(GPIOB, I2C_SCL_PIN);
        board_delay_us(2u);
    }

    GPIO_ResetBits(GPIOB, I2C_SDA_PIN);
    board_delay_us(2u);
    GPIO_SetBits(GPIOB, I2C_SCL_PIN);
    board_delay_us(2u);
    GPIO_SetBits(GPIOB, I2C_SDA_PIN);
    board_delay_us(2u);

    i2c_gpio_config(GPIO_Mode_AF_OD);
}
/* I2C 外设初始化，400kHz，复用开漏。 */
static void i2c_peripheral_init(void)
{
    I2C_InitTypeDef i2c;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    i2c_gpio_config(GPIO_Mode_AF_OD);

    I2C_DeInit(I2C1);
    I2C_StructInit(&i2c);
    i2c.I2C_ClockSpeed          = 400000u;
    i2c.I2C_Mode                = I2C_Mode_I2C;
    // 50% 占空比，400kHz 时钟周期 2.5us，SCL 高电平时间 1.25us，低电平时间 1.25us。
    i2c.I2C_DutyCycle           = I2C_DutyCycle_2;
    i2c.I2C_OwnAddress1         = 0u;
    i2c.I2C_Ack                 = I2C_Ack_Enable;
    // 7 位地址模式，LDC1614 的 I2C 地址是 0x50。
    i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &i2c);
    I2C_Cmd(I2C1, ENABLE);
    I2C_AcknowledgeConfig(I2C1, ENABLE);
}

static void abort_transfer(void)
{
    I2C_GenerateSTOP(I2C1, ENABLE);
    clear_error_flags();
    board_delay_ms(1u);

    i2c_bus_recover();
    i2c_peripheral_init();
}
/* 等待 I2C1 总线空闲，发送起始条件。 */
static int start_initial(void)
{
    int rc;

    rc = wait_flag_clear(I2C_FLAG_BUSY);
    if (rc != 0) {
        return rc;
    }

    I2C_GenerateSTART(I2C1, ENABLE);
    return wait_flag_set(I2C_FLAG_SB);
}

static int start_repeated(void)
{
    I2C_GenerateSTART(I2C1, ENABLE);
    return wait_flag_set(I2C_FLAG_SB);
}
// 发送写地址，等待 ACK，返回 0 成功，负数表示 NACK、超时或总线错误。
static int send_write_address(uint8_t addr7)
{
    int rc;

    I2C_Send7bitAddress(I2C1, (uint8_t)(addr7 << 1), I2C_Direction_Transmitter);
    rc = wait_flag_set(I2C_FLAG_ADDR);
    if (rc != 0) {
        return rc;
    }

    clear_addr_flag();
    return 0;
}

// 发送读地址，等待 ACK，返回 0 成功，负数表示 NACK、超时或总线错误。
static int send_read_address(uint8_t addr7, uint16_t rlen)
{
    int rc;

    I2C_Send7bitAddress(I2C1, (uint8_t)(addr7 << 1), I2C_Direction_Receiver);
    rc = wait_flag_set(I2C_FLAG_ADDR);
    if (rc != 0) {
        return rc;
    }

    /* 收 1 个字节: 收完就得 NACK, 所以要先把 ACK 关掉。
     * 收 2 个字节: ACK 先开着, 等第一个字节收到后再关(在 read_payload 里关)。
     * 原来收 2 字节用的是 POS=1 + 等 BTF 那套写法, 实测只能读对高字节、
     * 低字节恒为 0xFF —— 这就是之前读 ID 一直失败的根源。 */
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);
    if (rlen == 1u) {
        I2C_AcknowledgeConfig(I2C1, DISABLE);
    } else {
        I2C_AcknowledgeConfig(I2C1, ENABLE);
    }

    clear_addr_flag();
    return 0;
}
//把每一个字节都写入，进行发送
static int write_payload(const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    int rc;

    for (i = 0u; i < len; i++) {
        rc = wait_flag_set(I2C_FLAG_TXE);
        if (rc != 0) {
            return rc;
        }
        I2C_SendData(I2C1, buf[i]);
    }

    return wait_flag_set(I2C_FLAG_BTF);
}
//发送停止条件，等待总线空闲,超时返回-1，错误返回-2
static int stop_transfer(void)
{
    I2C_GenerateSTOP(I2C1, ENABLE);
    return wait_flag_clear(I2C_FLAG_BUSY);
}

static int read_payload(uint8_t *buf, uint16_t len)
{
    int rc;
    uint16_t i;

    if (len != 1u && len != 2u) {
        return -3;
    }

    /* 一个字节一个字节收: 读完倒数第二个字节就把 ACK 关掉,
     * 让最后一个字节收到 NACK, 然后发 STOP。这套写法在自检里对过,
     * 能稳定读到 0x5449 / 0x3055 / 0x2801 这些正确的寄存器值。 */
    for (i = 0u; i < len; i++) {
        rc = wait_flag_set(I2C_FLAG_RXNE);
        if (rc != 0) {
            break;
        }
        buf[i] = I2C_ReceiveData(I2C1);

        if ((uint16_t)(i + 2u) == len) {
            /* 下一个字节就是最后一个, 提前把 ACK 关掉 */
            I2C_AcknowledgeConfig(I2C1, DISABLE);
        }
        if ((uint16_t)(i + 1u) == len) {
            I2C_GenerateSTOP(I2C1, ENABLE);
        }
    }

    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);
    I2C_AcknowledgeConfig(I2C1, ENABLE);

    if (rc != 0) {
        return rc;
    }
    return wait_flag_clear(I2C_FLAG_BUSY);
}

void hard_i2c_init(void)
{
    i2c_peripheral_init();
}
/* 调试用: 把 PB6/PB7 临时切成输入, 用内部下拉和内部上拉各读一次。
 * 返回 bit0/1 = 内部下拉时 SCL(PB6)/SDA(PB7) 的电平,
 *      bit2/3 = 内部上拉时 SCL/SDA 的电平。
 * 下拉读到 1 -> 外部上拉把线拉高了, 模块供电和上拉正常;
 * 下拉 0 而上拉 1 -> 线上根本没有上拉(模块没供电/线没接上);
 * 上拉也读到 0 -> 这根线被拉到地了(短路)。 */
uint8_t hard_i2c_lines(void)
{
    uint8_t v = 0u;

    I2C_Cmd(I2C1, DISABLE);

    i2c_gpio_config(GPIO_Mode_IPD);
    board_delay_us(50u);

    if ((GPIOB->IDR & (uint32_t)I2C_SCL_PIN) != 0u) {
        v |= 1u;
    }
    if ((GPIOB->IDR & (uint32_t)I2C_SDA_PIN) != 0u) {
        v |= 2u;
    }

    i2c_gpio_config(GPIO_Mode_IPU);
    board_delay_us(50u);

    if ((GPIOB->IDR & (uint32_t)I2C_SCL_PIN) != 0u) {
        v |= 4u;
    }
    if ((GPIOB->IDR & (uint32_t)I2C_SDA_PIN) != 0u) {
        v |= 8u;
    }

    i2c_gpio_config(GPIO_Mode_AF_OD);
    I2C_Cmd(I2C1, ENABLE);

    return v;
}
//写，成功了返回 0，失败返回负数，-1表示总线错误，-2表示从机 NACK，-3表示参数错误
int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
    int rc;

    if (buf == 0 && len != 0u) {
        return -3;
    }

    clear_error_flags();

    rc = start_initial();
    if (rc == 0) {
        // 发送写地址
        rc = send_write_address(addr7);
    }
    if (rc == 0 && len != 0u) {
        // 发送数据
        rc = write_payload(buf, len);
    }
    if (rc == 0) {
        // 发送停止条件
        rc = stop_transfer();
    }

    if (rc != 0) {
        // 发生错误，中止传输
        abort_transfer();
        return (rc == -2) ? -2 : -1;
    }
    return 0;
}
// 寄存器读时序: 写寄存器地址 -> 重复起始 -> 读 1 或 2 字节。
int hard_i2c_write_read(uint8_t addr7,
                        const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen)
{
    int rc;
//检查参数，写缓冲区为空但长度不为 0，读缓冲区为空，读长度不是 1 或 2 都是错误
    if ((wbuf == 0 && wlen != 0u) || rbuf == 0 ||
        (rlen != 1u && rlen != 2u)) {
        return -3;
    }

    clear_error_flags();

    rc = start_initial();
    // 发送写地址,从机地址
    if (rc == 0) {
        rc = send_write_address(addr7);
    }
    // 发送写缓冲区数据，就是读的寄存器地址
    if (rc == 0 && wlen != 0u) {
        rc = write_payload(wbuf, wlen);
    }
    // 发送重复起始条件
    if (rc == 0) {
        rc = start_repeated();
    }
    // 发送读地址
    if (rc == 0) {
        rc = send_read_address(addr7, rlen);
    }
    // 读取数据
    if (rc == 0) {
        rc = read_payload(rbuf, rlen);
    }
    // 发送停止条件
    if (rc != 0) {
        abort_transfer();
        return (rc == -2) ? -2 : -1;
    }
    return 0;
}
