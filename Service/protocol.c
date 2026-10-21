/*
 * protocol.c - 串口协议实现
 *
 * 全部用定点数与自写的字符串拼接, 不用 printf / strtof,
 * 这样 Keil (microlib) 上不用开浮点格式化也能正常输出。
 */

#include <string.h>
#include "protocol.h"
#include "board.h"
#include "ldc1614.h"

static uint32_t s_rate_ms = 100u;
static uint32_t s_last_ms;

void protocol_init(void)
{
    s_rate_ms = 100u;//默认 100ms 上报一次
    s_last_ms = board_millis();
}
//设置上报间隔，单位毫秒
void protocol_set_rate_ms(uint32_t ms)
{
    s_rate_ms = ms;
    s_last_ms = board_millis();
}
//获取上报间隔，单位毫秒
uint32_t protocol_get_rate_ms(void)
{
    return s_rate_ms;
}

/* ------------------------------------------------------------------ */
/* 字符串工具                                                           */
/* ------------------------------------------------------------------ */
//out: 输出缓冲区, pos: 当前写入位置, cap: 缓冲区容量, c: 要写入的字符,返回的是新的写入位置,用于输出标点符号
static int put_char(char *out, int pos, int cap, char c)
{
    if (pos < (cap - 1)) {
        out[pos++] = c;
        out[pos] = '\0';
    }
    return pos;
}
//out: 输出缓冲区, pos: 当前写入位置, cap: 缓冲区容量, s: 要写入的字符串
static int put_str(char *out, int pos, int cap, const char *s)
{
    while (*s != '\0') {
        pos = put_char(out, pos, cap, *s++);
    }
    return pos;
}
//把 0~4294967295 的整数打印成字符串
static int put_u32(char *out, int pos, int cap, uint32_t v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);

    while (n > 0) {
        pos = put_char(out, pos, cap, tmp[--n]);
    }
    return pos;
}

/* 把放大 10^frac 倍的整数打印成定点小数,
 *例如 523,3 -> "0.523"
 *与float_to_scaled（）这个函数一起用相当于四舍五入 */
static int put_fixed(char *out, int pos, int cap, int32_t value, int frac)
{
    int32_t scale = 1;
    int neg = 0;
    int i;

    for (i = 0; i < frac; i++) {
        scale *= 10;
    }
    if (value < 0) {
        neg = 1;
        value = -value;
    }

    if (neg) {
        pos = put_char(out, pos, cap, '-');
    }
    pos = put_u32(out, pos, cap, (uint32_t)(value / scale));
    pos = put_char(out, pos, cap, '.');

    for (i = frac - 1; i >= 0; i--) {
        int32_t div = 1;
        int k;
        for (k = 0; k < i; k++) {
            div *= 10;
        }
        pos = put_char(out, pos, cap, (char)('0' + (int)((value / div) % 10)));
    }
    return pos;
}
//out: 输出缓冲区, pos: 当前写入位置, cap: 缓冲区容量, 写入回车换行,用于换车换行
static int put_crlf(char *out, int pos, int cap)
{
    pos = put_char(out, pos, cap, '\r');
    return put_char(out, pos, cap, '\n');
}
//float 转定点整数, scale = 1000 -> 3 位小数, scale = 10 -> 1 位小数
static int32_t float_to_scaled(float f, int scale)
{
    float v = f * (float)scale;
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));/*四舍五入,int32_t强制类型转换达到四舍五入*/
}

/* "0.800" -> 800 (frac = 3); 允许省略小数部分 */
static int parse_fixed(const char *s, int frac, int32_t *out)
{
    int neg = 0;
    int digits = 0;
    int32_t v = 0;

    if (s == 0 || *s == '\0') {
        return -1;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = (v * 10) + (int32_t)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (digits < frac) {
                v = (v * 10) + (int32_t)(*s - '0');
                digits++;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    while (digits < frac) {
        v *= 10;
        digits++;
    }

    *out = neg ? -v : v;
    return 0;
}

/* 只接受 0~4294967295 的纯数字 */
static int parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0;

    if (s == 0) {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        v = (v * 10u) + (uint32_t)(*s - '0');
        s++;
        digits++;
    }
    if (digits == 0 || *s != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

/* 十六进制 (RD/WR 用), 最多 4 位 */
static int parse_hex(const char *s, uint16_t *out)
{
    uint16_t v = 0;
    int digits = 0;

    if (s == 0) {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (digits < 4) {
        char c = *s;
        uint16_t d;
        if (c >= '0' && c <= '9') {
            d = (uint16_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint16_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint16_t)(c - 'A' + 10);
        } else {
            break;
        }
        v = (uint16_t)((v << 4) | d);
        s++;
        digits++;
    }
    if (digits == 0 || *s != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}
//out: 输出缓冲区, cap: 缓冲区容量, code: 错误码
static int reply_ok(char *out, int cap)
{
    int p = put_str(out, 0, cap, "OK");
    return put_crlf(out, p, cap);
}
//out: 输出缓冲区, cap: 缓冲区容量, code: 错误码
static int reply_err(char *out, int cap, int code)
{
    int p = put_str(out, 0, cap, "ERR,");
    p = put_u32(out, p, cap, (uint32_t)code);
    return put_crlf(out, p, cap);
}

/* ------------------------------------------------------------------ */
/* 数据帧                                                               */
/* ------------------------------------------------------------------ */
// 到上报时间则生成一帧 $D 写入 out, 返回字节数; 未到返回 0 */
/*
*   输出为       $D, 5.231 , 25.4 , 0
*   $D          表示一条距离数据
*   5.231       距离，单位 mm
*   25.4        温度，单位摄氏度
*   0           状态，正常
*/
int protocol_build_d(char *out, int outlen, const measure_result_t *r)
{
    int p = 0;

    if (out == 0 || outlen <= 0 || r == 0) {
        return 0;
    }

    p = put_str(out, p, outlen, "$D,");
    p = put_fixed(out, p, outlen, float_to_scaled(r->dist_mm, 1000), 3);
    p = put_char(out, p, outlen, ',');
    p = put_fixed(out, p, outlen, float_to_scaled(r->temp_c, 10), 1);
    p = put_char(out, p, outlen, ',');
    p = put_u32(out, p, outlen, (uint32_t)r->status);
    return put_crlf(out, p, outlen);
}
// $R,<ch0>,<ch1>,<比值>,<间隙mm>,<温度C>,<状态>\r\n,输出原始数据帧
int protocol_build_r(char *out, int outlen, const measure_result_t *r)
{
    int p = 0;

    if (out == 0 || outlen <= 0 || r == 0) {
        return 0;
    }

    p = put_str(out, p, outlen, "$R,");
    p = put_u32(out, p, outlen, r->ch0);
    p = put_char(out, p, outlen, ',');
    p = put_u32(out, p, outlen, r->ch1);
    p = put_char(out, p, outlen, ',');
    p = put_fixed(out, p, outlen, float_to_scaled(r->ratio, 100000), 5);
    p = put_char(out, p, outlen, ',');
    p = put_fixed(out, p, outlen, float_to_scaled(r->dist_mm, 1000), 3);
    p = put_char(out, p, outlen, ',');
    p = put_fixed(out, p, outlen, float_to_scaled(r->temp_c, 10), 1);
    p = put_char(out, p, outlen, ',');
    p = put_u32(out, p, outlen, (uint32_t)r->status);
    return put_crlf(out, p, outlen);
}
// 检查是否到上报时间，到则生成一帧 $R 写入 out, 返回字节数; 未到返回 0
int protocol_tick(char *out, int outlen, const measure_result_t *r, uint32_t now_ms)
{
    if (s_rate_ms == 0u) {
        return 0;
    }
    if ((uint32_t)(now_ms - s_last_ms) < s_rate_ms) {
        return 0;
    }
    s_last_ms = now_ms;
    return protocol_build_r(out, outlen, r);
}

/* ------------------------------------------------------------------ */
/* 命令解析                                                             */
/* ------------------------------------------------------------------ */

#define CMD_MAX_TOKENS   6
/*分割命令行, 以逗号为分隔符, tok[0] = 命令名, 返回分割出的 token 个数
*按逗号拆解命令符串,例如 "RATE,100" 拆解为 tok[0] = "RATE", tok[1] = "100", 返回 2
*/
static int split_tokens(char *line, char *tok[CMD_MAX_TOKENS])
{
    int n = 0;
    char *p = line;

    while (n < CMD_MAX_TOKENS) {
        tok[n++] = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        *p++ = '\0';
    }
    return n;
}
/*回复 INFO, 包含版本号、上报间隔、标定点个数、报警/提醒阈值、温补系数等
*相当于查找当前配置参数
*/
static int reply_info(char *out, int cap, const measure_result_t *r)
{
    float alarm;
    float warn;
    float hyst;
    float k;
    float tref;
    int i;
    int p = 0;

    measure_get_thresholds(&alarm, &warn, &hyst);
    measure_get_temp_comp(&k, &tref);

    p = put_str(out, p, cap, "INFO,v");
    p = put_str(out, p, cap, PROTO_VERSION);
    p = put_str(out, p, cap, ",rate=");
    p = put_u32(out, p, cap, s_rate_ms);
    p = put_str(out, p, cap, ",N=");
    p = put_u32(out, p, cap, (uint32_t)measure_cal_count());
    p = put_str(out, p, cap, ",A=");
    p = put_fixed(out, p, cap, float_to_scaled(alarm, 1000), 3);
    p = put_str(out, p, cap, ",W=");
    p = put_fixed(out, p, cap, float_to_scaled(warn, 1000), 3);
    p = put_str(out, p, cap, ",H=");
    p = put_fixed(out, p, cap, float_to_scaled(hyst, 1000), 3);
    p = put_str(out, p, cap, ",swap=");
    p = put_u32(out, p, cap, (uint32_t)measure_get_swap());
    p = put_str(out, p, cap, ",k=");
    p = put_fixed(out, p, cap, float_to_scaled(k, 1000000), 6);
    p = put_str(out, p, cap, ",Tref=");
    p = put_fixed(out, p, cap, float_to_scaled(tref, 10), 1);

    p = put_str(out, p, cap, ",P=");
    for (i = 0; i < measure_cal_count(); i++) {
        float gap;
        float ratio;
        if (measure_cal_get(i, &gap, &ratio) != 0) {
            break;
        }
        p = put_fixed(out, p, cap, float_to_scaled(gap, 1000), 3);
        p = put_char(out, p, cap, '/');
        p = put_fixed(out, p, cap, float_to_scaled(ratio, 100000), 5);
        p = put_char(out, p, cap, ';');
    }

    if (r != 0) {
        p = put_str(out, p, cap, ",D=");
        p = put_fixed(out, p, cap, float_to_scaled(r->dist_mm, 1000), 3);
        p = put_str(out, p, cap, ",S=");
        p = put_u32(out, p, cap, (uint32_t)r->status);
    }
    return put_crlf(out, p, cap);
}
// 检查寄存器地址是否合法, 只允许访问 0x00~0x1C, 0x1E~0x21, 0x7E~0x7F
static int reg_is_valid(uint16_t reg)
{
    if (reg <= 0x1Cu) {
        return 1;
    }
    if (reg >= 0x1Eu && reg <= 0x21u) {
        return 1;
    }
    if (reg == LDC1614_REG_MANUFACTURER_ID || reg == LDC1614_REG_DEVICE_ID) {
        return 1;
    }
    return 0;
}

/* RD,<reg> / WR,<reg>,<val> 直接读写 LDC1614 寄存器, 用于台架调试
*台架调试是指在实验室环境下对设备进行测试和调试,
*通过串口协议发送 RD 或 WR 命令来读取或写入 LDC1614 的寄存器值,
*以便验证设备的功能和性能, 并进行必要的调整和优化
*相当于处理寄存器读写命令, RD 读取寄存器值, WR 写入寄存器值
*/
static int handle_reg_command(const char *name, char *tok[], int n,
                              char *out, int cap)
{
    uint16_t reg;
    uint16_t val;

    if (parse_hex(tok[1], &reg) != 0 || !reg_is_valid(reg)) {
        return reply_err(out, cap, 1);
    }

    if (strcmp(name, "RD") == 0) {
        if (ldc1614_read_reg((uint8_t)reg, &val) != 0) {
            return reply_err(out, cap, 4);
        }
        {
            int p = put_str(out, 0, cap, "REG,");
            p = put_u32(out, p, cap, reg);
            p = put_char(out, p, cap, ',');
            p = put_u32(out, p, cap, val);
            return put_crlf(out, p, cap);
        }
    }

    if (reg == LDC1614_REG_MANUFACTURER_ID || reg == LDC1614_REG_DEVICE_ID) {
        return reply_err(out, cap, 1);    /* ID 寄存器只读 */
    }
    if (n < 3) {
        return reply_err(out, cap, 1);
    }
    if (parse_hex(tok[2], &val) != 0) {
        return reply_err(out, cap, 1);
    }
    if (ldc1614_write_reg((uint8_t)reg, val) != 0) {
        return reply_err(out, cap, 4);
    }
    return reply_ok(out, cap);
}
/*处理 DUMP 命令, 读取所有寄存器并返回
*用于调试和验证设备的状态和配置, 以便进行故障排除和性能优化
*输出格式为 DUMP,<reg0>,<reg1>,...,<reg33>,ID,<mid>,<did>\r\n
*其中 <reg0>~<reg33> 为 LDC1614 的寄存器值, <mid> 为制造商 ID, <did> 为设备 ID
*/
static int handle_dump(char *out, int cap)
{
    uint8_t regs[34];
    uint16_t mid = 0u;
    uint16_t did = 0u;
    int i;
    int p;

    if (ldc1614_dump_regs(regs, 34u) != 0 ||
        ldc1614_read_reg(LDC1614_REG_MANUFACTURER_ID, &mid) != 0 ||
        ldc1614_read_reg(LDC1614_REG_DEVICE_ID, &did) != 0) {
        return reply_err(out, cap, 4);
    }

    p = put_str(out, 0, cap, "DUMP,");
    for (i = 0; i < 34; i++) {
        p = put_u32(out, p, cap, regs[i]);
        if (i < 33) {
            p = put_char(out, p, cap, ',');
        }
    }
    p = put_str(out, p, cap, ",ID,");
    p = put_u32(out, p, cap, mid);
    p = put_char(out, p, cap, ',');
    p = put_u32(out, p, cap, did);
    return put_crlf(out, p, cap);
}
/* 处理一条完整命令行, 回复写入 out, 返回回复字节数 (0 = 不应答)
* 命令行格式: <CMD>[,<arg1>[,<arg2>...]]\r\n
* 支持的命令:
*   READ       读取距离数据, 回复 $D,<dist_mm>,<temp_c>,<status>\r\n
*   RAW        读取原始数据, 回复 $R,<ch0>,<ch1>,<ratio>,<dist_mm>,<temp_c>,<status>\r\n
*   INFO       查询设备信息,
*               回复 INFO,v<ver>,rate=<ms>,N=<cal_count>,A=<alarm_mm>,W=<warn_mm>,H=<hyst_mm>,
*                         swap=<0|1>,k=<temp_comp_k>,Tref=<temp    _comp_ref>,P=<cal_points>\r\n
*   CAL,<n>,<gap_mm>  设置标定点 n (1~MEAS_MAX_POINTS), 间隙 mm, 回复 OK\r\n
*   CALS      保存标定点到 Flash, 回复 OK\r\n
*   CLR        清除所有 标定点, 回复 OK\r\n
*   DUMP       读取所有寄存器, 回复 DUMP,<reg0>,<reg1>,...,<reg33>,ID,<mid>,<did>\r\n
*   RD,<reg>    读取寄存器值, 回复 REG,<reg>,<val>\r\n
*   WR,<reg>,<val>  写入寄存器值, 回复 OK\r\n
*   RATE,<ms>  设置上报间隔 ms, 回复 OK\r\n
*   SWAP,<0|1> 设置通道交换, 回复 OK\r\n
*   STX,<alarm_mm>  设置报警阈值 mm, 回复 OK\r\n
*   ST2,<warn_mm>   设置提醒阈值 mm, 回复 OK\r\n
*   TC,<k>,<tref>   设置温补系数 k 和参考温度 tref, 回复 OK\r\n
*   其他命令或参数错误, 回复 ERR,<code>\r\n
*   串口中断的时候调用 protocol_handle_line() 处理命令行, 并把回复写入串口
*/
int protocol_handle_line(const char *line, char *out, int outlen)
{
    char buf[96];
    char *tok[CMD_MAX_TOKENS];
    int n;
    uint32_t u;
    int32_t v;
    const measure_result_t *r = measure_latest();

    if (line == 0 || out == 0 || outlen <= 0) {
        return 0;
    }

    strncpy(buf, line, sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';

    n = split_tokens(buf, tok);
    if (n == 0 || tok[0][0] == '\0') {
        return 0;
    }

    if (strcmp(tok[0], "READ") == 0) {
        return protocol_build_d(out, outlen, r);
    }
    if (strcmp(tok[0], "RAW") == 0) {
        return protocol_build_r(out, outlen, r);
    }
    if (strcmp(tok[0], "INFO") == 0) {
        return reply_info(out, outlen, r);
    }
    if (strcmp(tok[0], "CALS") == 0) {
        measure_save();
        return reply_ok(out, outlen);
    }
    if (strcmp(tok[0], "CLR") == 0) {
        measure_cal_clear();
        measure_save();
        return reply_ok(out, outlen);
    }
    if (strcmp(tok[0], "DUMP") == 0) {
        return handle_dump(out, outlen);
    }
    if ((strcmp(tok[0], "RD") == 0 || strcmp(tok[0], "WR") == 0) && n >= 2) {
        return handle_reg_command(tok[0], tok, n, out, outlen);
    }

    if (strcmp(tok[0], "RATE") == 0 && n >= 2) {
        if (parse_u32(tok[1], &u) != 0 || u > 60000u) {
            return reply_err(out, outlen, 1);
        }
        protocol_set_rate_ms(u);
        return reply_ok(out, outlen);
    }

    if (strcmp(tok[0], "SWAP") == 0 && n >= 2) {
        if (parse_u32(tok[1], &u) != 0 || u > 1u) {
            return reply_err(out, outlen, 1);
        }
        measure_set_swap((uint8_t)u);
        return reply_ok(out, outlen);
    }

    if (strcmp(tok[0], "STX") == 0 && n >= 2) {
        float a;
        float w;
        float h;
        if (parse_fixed(tok[1], 3, &v) != 0) {
            return reply_err(out, outlen, 1);
        }
        measure_get_thresholds(&a, &w, &h);
        measure_set_thresholds((float)v / 1000.0f, w, h);
        return reply_ok(out, outlen);
    }

    if (strcmp(tok[0], "ST2") == 0 && n >= 2) {
        float a;
        float w;
        float h;
        if (parse_fixed(tok[1], 3, &v) != 0) {
            return reply_err(out, outlen, 1);
        }
        measure_get_thresholds(&a, &w, &h);
        measure_set_thresholds(a, (float)v / 1000.0f, h);
        return reply_ok(out, outlen);
    }

    if (strcmp(tok[0], "TC") == 0 && n >= 3) {
        float k;
        float tref;
        if (parse_fixed(tok[1], 6, &v) != 0) {
            return reply_err(out, outlen, 1);
        }
        k = (float)v / 1000000.0f;
        if (parse_fixed(tok[2], 1, &v) != 0) {
            return reply_err(out, outlen, 1);
        }
        tref = (float)v / 10.0f;
        measure_set_temp_comp(k, tref);
        return reply_ok(out, outlen);
    }

    if (strcmp(tok[0], "CAL") == 0 && n >= 3) {
        int rc;
        if (parse_u32(tok[1], &u) != 0 || u < 1u || u > (uint32_t)MEAS_MAX_POINTS) {
            return reply_err(out, outlen, 1);
        }
        if (parse_fixed(tok[2], 3, &v) != 0) {
            return reply_err(out, outlen, 1);
        }
        rc = measure_cal_set((int)u, (float)v / 1000.0f);
        if (rc != 0) {
            return reply_err(out, outlen, (rc == -3) ? 2 : 3);
        }
        return reply_ok(out, outlen);
    }

    return reply_err(out, outlen, 9);
}
