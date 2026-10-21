/*
 * app.c - 应用层实现
 */

#include <string.h>
#include "app.h"
#include "board.h"
#include "uart.h"
#include "adc_temp.h"
#include "ldc1614.h"
#include "measure.h"
#include "protocol.h"
#include "oled.h"
#include "button.h"

/* ---- 可调参数 ---- */
#define APP_BAUD          115200u   /* 上位机串口波特率, 接 CH340 */
#define APP_REPORT_MS     100u      /* 周期上报, 100ms */
#define APP_TEMP_MS       1000u     /* 温度每秒读一次 */
#define APP_INTB_TIMEOUT  500u      /* 超过这么久没有 INTB 就判故障 */
#define APP_OLED_MS       250u      /* OLED 刷新周期, 别太快, 免得占满 I2C */
#define APP_I2C_DEBUG     1         /* 1 = LDC 初始化失败时打总线诊断, 调好后改 0 */

static char s_out[192];
static uint32_t s_last_intb_ms;/*上次收到 INTB 的时间戳*/
static uint32_t s_last_temp_ms;
static uint32_t s_last_oled_ms;
static float    s_temp_c;
static uint8_t  s_oled_ok;     /*1 = OLED 正常应答*/
static uint8_t  s_page;        /*0 = 主界面, 1 = 调试界面*/
static uint8_t  s_hold;        /*1 = 冻结显示*/
static measure_result_t s_show;/*OLED 正在显示的一份数据*/

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

/* ------------------------------------------------------------------ */
/* OLED 显示                                                            */
/* ------------------------------------------------------------------ */

static const char *status_text(uint8_t st)
{
    switch (st) {
    case 0u: return "NORMAL";
    case 1u: return "WARN";
    case 2u: return "ALARM";
    case 3u: return "FAULT";
    default: return "UNCAL";
    }
}
//无符号整数转十进制字符串
static void fmt_u32(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);
    while (n > 0) {
        *buf++ = tmp[--n];
    }
    *buf = '\0';
}
//浮点数转固定小数位字符串, 例如 fmt_fixed(buf, 0.52f, 2) -> "0.52"
static void fmt_fixed(char *buf, float v, uint8_t dec)
{
    static const uint32_t P10[6] = {1u, 10u, 100u, 1000u, 10000u, 100000u};
    uint32_t scale = P10[dec];
    int32_t scaled;
    uint32_t u;
    char tmp[12];
    int n = 0;
    int i;
    char *p = buf;

    scaled = (int32_t)((v >= 0.0f) ? (v * (float)scale + 0.5f)
                                   : (v * (float)scale - 0.5f));
    if (scaled < 0) {
        *p++ = '-';
        u = (uint32_t)(-scaled);
    } else {
        u = (uint32_t)scaled;
    }

    do {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u > 0u);
    while (n <= (int)dec) {
        tmp[n++] = '0';
    }

    for (i = n - 1; i >= 0; i--) {
        if (dec > 0u && i == (int)dec - 1) {
            *p++ = '.';
        }
        *p++ = tmp[i];
    }
    *p = '\0';
}

/* 主界面: 距离(大字) + 温度 + 状态 */
static void draw_main(const measure_result_t *r)
{
    char num[12];

    oled_draw_str(0, 0, "GAP", 1);
    oled_draw_str_right(OLED_WIDTH - 1, 0, status_text(r->status), 1);

    if (r->valid != 0u) {
        fmt_fixed(num, r->dist_mm, 2);
    } else {
        strcpy(num, "--.--");
    }
    oled_draw_str(0, 14, num, 3);
    oled_draw_str(oled_text_width(num, 3) + 4, 30, "MM", 1);

    oled_draw_str(0, 40, "T", 1);
    fmt_fixed(num, r->temp_c, 1);
    oled_draw_str(12, 40, num, 1);
    oled_draw_str(12 + oled_text_width(num, 1) + 3, 40, "C", 1);

    oled_draw_str(0, 52, "R", 1);
    fmt_fixed(num, r->ratio, 5);
    oled_draw_str(12, 52, num, 1);

    if (s_hold != 0u) {
        oled_draw_str_right(OLED_WIDTH - 1, 52, "HOLD", 1);
    }
}

/* 调试界面: 原始值 + 比值 */
static void draw_debug(const measure_result_t *r)
{
    char buf[16];

    oled_draw_str(0, 0, "DEBUG", 1);

    oled_draw_str(0, 12, "CH0", 1);
    fmt_u32(buf, r->ch0);
    oled_draw_str(30, 12, buf, 1);

    oled_draw_str(0, 22, "CH1", 1);
    fmt_u32(buf, r->ch1);
    oled_draw_str(30, 22, buf, 1);

    oled_draw_str(0, 32, "RATIO", 1);
    fmt_fixed(buf, r->ratio, 5);
    oled_draw_str(42, 32, buf, 1);

    oled_draw_str(0, 42, "TEMP", 1);
    fmt_fixed(buf, r->temp_c, 1);
    oled_draw_str(42, 42, buf, 1);

    oled_draw_str(0, 52, "ST", 1);
    oled_draw_str(18, 52, status_text(r->status), 1);

    if (s_hold != 0u) {
        oled_draw_str_right(OLED_WIDTH - 1, 52, "HOLD", 1);
    }
}

static void display_update(void)
{
    if (s_oled_ok == 0u) {
        return;
    }

    if (s_hold == 0u) {
        s_show = *measure_latest();
    }

    oled_clear();
    if (s_page == 0u) {
        draw_main(&s_show);
    } else {
        draw_debug(&s_show);
    }
    oled_flush();
}

/* ------------------------------------------------------------------ */

void app_init(void)
{
    int rc;

    board_init();
    uart_init(APP_BAUD);
    button_init();

    /* 开机先报一句: 串口通没通、时钟是 72MHz 还是退到了 HSI 8MHz。
     * 后面不管卡在哪个模块, 至少这几行能说明前面是好的。 */
    uart_puts(board_clock_is_72m() ? "$B,BOOT,72M\r\n" : "$B,BOOT,HSI\r\n");

    ldc1614_init_driver();/*就是i2c硬件初始化*/

    /* OLED 和 LDC1614 挂在同一条 I2C 总线上 */
    s_oled_ok = (oled_init() == 0) ? 1u : 0u;
    uart_puts(s_oled_ok ? "$B,OLED OK\r\n" : "$B,OLED FAIL\r\n");

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
    s_last_intb_ms = s_last_temp_ms;/*记录上次收到 INTB 的时间戳*/

    if (rc == 0) {
        uart_puts("$B,LDC1614 ready\r\n");
    }

    s_show = *measure_latest();/*先把当前(未标定)状态拿来画一屏*/
    s_last_oled_ms = board_millis();
    display_update();
}

void app_poll(void)
{
    uint32_t now = board_millis();/*现在的时间戳*/

    /* ---- 0. 按键 ---- */
    button_scan(now);

    if (button_short_press(BTN_KEY1)) {
        s_page = (uint8_t)((s_page == 0u) ? 1u : 0u);/*主界面 <-> 调试界面*/
        display_update();
    }
    if (button_short_press(BTN_KEY2)) {
        s_hold = (uint8_t)((s_hold == 0u) ? 1u : 0u);/*冻结/恢复显示*/
        display_update();
    }

    /* ---- 1. 采样: INTB 到了就读一次 ---- */
    if (board_intb_take()) {
        uint32_t ch0 = 0u;
        uint32_t ch1 = 0u;
        uint16_t err = 0u;

        s_last_intb_ms = now;//更新上次INTB中断的时间戳

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

    /* ---- 2. INTB 长时间不来 -> 故障 ---- */
    if ((uint32_t)(now - s_last_intb_ms) > APP_INTB_TIMEOUT) {
        measure_fault();
    }

    /* ---- 3. OLED 刷新: 250ms 一次, 别把 I2C 占满 ---- */
    if ((uint32_t)(now - s_last_oled_ms) >= APP_OLED_MS) {
        s_last_oled_ms = now;
        display_update();
    }

    /* ---- 4. 周期上报 ---- */
    send(s_out, protocol_tick(s_out, (int)sizeof(s_out), measure_latest(), now));

    board_feed_watchdog();
}
