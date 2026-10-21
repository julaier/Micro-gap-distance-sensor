/*
 * app.c - 应用层实现
 */

#include "app.h"
#include "board.h"
#include "uart.h"
#include "adc_temp.h"
#include "ldc1614.h"
#include "measure.h"
#include "protocol.h"

/* ---- 可调参数 ---- */
#define APP_BAUD          115200u   /* 上位机串口波特率, 接 CH340 */
#define APP_REPORT_MS     100u      /* 周期上报, 100ms */
#define APP_TEMP_MS       1000u     /* 温度每秒读一次 */
#define APP_POLL_MS       20u       /* INTB 没接到也能跑: 每 20ms 主动读一次原始值 */
#define APP_DATA_TIMEOUT  500u      /* 超过这么久没读到数据就判故障 */
#define APP_I2C_DEBUG     1         /* 1 = LDC 初始化失败时打总线诊断, 调好后改 0 */

static char s_out[192];
static uint32_t s_last_data_ms;/*上次成功读到原始值的时间戳*/
static uint32_t s_last_poll_ms;/*上次主动轮询的时间戳*/
static uint32_t s_last_temp_ms;
static float    s_temp_c;
/*用于发送数据*/
static void send(const char *buf, int len)
{
    if (len > 0) {
        uart_write((const uint8_t *)buf, (uint16_t)len);
    }
}

/* 把字符串追加到 buf, 返回写了几个字节 */
static int append_str(char *p, const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        p[n] = s[n];
        n++;
    }
    return n;
}
//把无符号整数按十进制追加到 buf, 返回新的写指针
static char *append_u32(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);
    while (n > 0) {
        *p++ = tmp[--n];
    }
    return p;
}
//带负号的版本
static char *append_i32(char *p, int32_t v)
{
    if (v < 0) {
        *p++ = '-';
        return append_u32(p, (uint32_t)(-v));
    }
    return append_u32(p, (uint32_t)v);
}

/* 温度明显异常时打一行原始值: $B,TEMP,cal=..,vref=..,ts=..,t10=.. */
static void report_temp_debug(int32_t t10)
{
    uint16_t cal = 0u;
    uint16_t vref = 0u;
    uint16_t ts = 0u;
    char *p = s_out;

    adc_temp_debug(&cal, &vref, &ts);

    p += append_str(p, "$B,TEMP,cal=");
    p = append_u32(p, cal);
    p += append_str(p, ",vref=");
    p = append_u32(p, vref);
    p += append_str(p, ",ts=");
    p = append_u32(p, ts);
    p += append_str(p, ",t10=");
    p = append_i32(p, t10);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

/* 初始化失败时发一个 "$E,<code>" 报文, 方便不看代码就知道卡在哪 */
static void report_boot_error(int code)
{
    int p = 0;
    char tmp[12];
    int n = 0;
    int v = -code;

    s_out[p++] = '$';
    s_out[p++] = 'E';
    s_out[p++] = ',';
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    while (n > 0) {
        s_out[p++] = tmp[--n];
    }
    s_out[p++] = '\r';
    s_out[p++] = '\n';
    send(s_out, p);
}

/* I2C 初始化失败时的总线诊断:
 *   $I,<pd>,<pu>     bit0 = SCL(PB6), bit1 = SDA(PB7)
 *                    pd: 内部下拉时读到的电平, 正常是 3 (外部上拉把线拉高)
 *                    pu: 内部上拉时读到的电平, 读到 0 说明线被短路到地
 *   $P,<addr>,<ack>  逐个地址探测, 1 = 从机应答, 0 = 无应答
 */
#if APP_I2C_DEBUG
static char hex_char(uint8_t v)
{
    return (char)((v < 10u) ? ('0' + v) : ('A' + v - 10u));
}

static void report_i2c_debug(void)
{
    static const uint8_t addrs[4] = {0x2Au, 0x2Bu, 0x2Cu, 0x2Du};
    uint8_t v = (uint8_t)ldc1614_i2c_lines();
    char line[16];
    uint8_t i;
    int p = 0;

    line[p++] = '$';
    line[p++] = 'I';
    line[p++] = ',';
    line[p++] = (char)('0' + (v & 0x03u));
    line[p++] = ',';
    line[p++] = (char)('0' + ((v >> 2) & 0x03u));
    line[p++] = '\r';
    line[p++] = '\n';
    send(line, p);

    for (i = 0u; i < 4u; i++) {
        p = 0;
        line[p++] = '$';
        line[p++] = 'P';
        line[p++] = ',';
        line[p++] = hex_char((uint8_t)(addrs[i] >> 4));
        line[p++] = hex_char((uint8_t)(addrs[i] & 0x0Fu));
        line[p++] = ',';
        line[p++] = (ldc1614_probe(addrs[i]) == 0) ? '1' : '0';
        line[p++] = '\r';
        line[p++] = '\n';
        send(line, p);
    }
}
#endif

void app_init(void)
{
    int rc;

    board_init();
    uart_init(APP_BAUD);

    /* 开机先报一句: 串口通没通、时钟是 72MHz 还是退到了 HSI 8MHz。
     * 后面不管卡在哪个模块, 至少这几行能说明前面是好的。 */
    uart_puts(board_clock_is_72m() ? "$B,BOOT,72M\r\n" : "$B,BOOT,HSI\r\n");

    ldc1614_init_driver();/*就是i2c硬件初始化*/
    adc_temp_init();

    measure_init();
    measure_load();

    protocol_init();/*初始化串口协议层,设置上报间隔为APP_REPORT_MS毫秒*/
    protocol_set_rate_ms(APP_REPORT_MS);/*设置上报间隔为APP_REPORT_MS毫秒，上面默认就是100这里可以不用*/

    rc = ldc1614_init(0, 0);/*初始化LDC1614，读取厂商ID,设备ID，这里为0也就是不读取*/
    if (rc != 0) {
        measure_fault();
#if APP_I2C_DEBUG
        report_i2c_debug();/*先报总线状态, 再报错误码*/
#endif
        report_boot_error(rc);/*汇报错误*/
    }

    s_temp_c = (float)adc_temp_read_x10() / 10.0f;/*读取温度传感器的温度值，单位为摄氏度*/
    {
        int32_t t10 = (int32_t)(s_temp_c * 10.0f);/*温度 x10, 用来判断是不是异常*/

        if (t10 < -400 || t10 > 1250) {
            report_temp_debug(t10);/*温度明显不对, 把原始值打出来方便查*/
        }
    }
    s_last_temp_ms = board_millis();/*记录上次读取温度的时间戳*/
    s_last_data_ms = s_last_temp_ms;/*记录上次读到数据的时间戳*/
    s_last_poll_ms = s_last_temp_ms;/*记录上次轮询的时间戳*/

    if (rc == 0) {
        uart_puts("$B,LDC1614 ready\r\n");
    }
}

void app_poll(void)
{
    uint32_t now = board_millis();/*现在的时间戳*/

    /* ---- 1. 采样 ----
     * INTB 触发就当快路径用; 但 INTB 那根线可能没接到 MCU,
     * 所以再加一条定时轮询兜底: 每 APP_POLL_MS 主动读一次原始值。
     * 两条路走同一段代码, 谁先来算谁。 */
    if (board_intb_take() ||
        (uint32_t)(now - s_last_poll_ms) >= APP_POLL_MS) {
        s_last_poll_ms = now;
        uint32_t ch0 = 0u;
        uint32_t ch1 = 0u;
        uint16_t err = 0u;

        s_last_data_ms = now;/*更新上次读到数据的时间戳*/

        if (ldc1614_read_raw(&ch0, &ch1, &err) == 0) {
            //如果距离上次读温度的时间大于APP_TEMP_MS，就再读一次温度，单位为摄氏度
            if ((uint32_t)(now - s_last_temp_ms) >= APP_TEMP_MS) {
                s_last_temp_ms = now;
                s_temp_c = (float)adc_temp_read_x10() / 10.0f;
            }
            measure_update(ch0, ch1, s_temp_c, now);
        } else {
            measure_fault();
        }
    }

    /* ---- 2. 长时间读不到数据 -> 故障 ---- */
    if ((uint32_t)(now - s_last_data_ms) > APP_DATA_TIMEOUT) {
        measure_fault();
    }

    /* ---- 3. 周期上报 ---- */
    send(s_out, protocol_tick(s_out, (int)sizeof(s_out), measure_latest(), now));

    board_feed_watchdog();
}
