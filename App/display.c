/*
 * display.c - OLED 显示层
 *
 * 显示布局和格式化都在这里, app.c 只调用 display_update()。
 */

#include <string.h>
#include "display.h"
#include "oled.h"
#include "measure.h"

static uint8_t s_oled_ok;      /*1 = OLED 正常应答*/
static uint8_t s_page;         /*0 = 主界面, 1 = 调试界面*/
static uint8_t s_hold;         /*1 = 冻结显示*/
static measure_result_t s_show;/*OLED 正在显示的一份数据*/

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
        fmt_fixed(num, r->dist_mm, 3);/*0.001mm 分辨率*/
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

int display_init(void)
{
    s_oled_ok = (oled_init() == 0) ? 1u : 0u;
    return (s_oled_ok != 0u) ? 0 : -1;
}

void display_toggle_page(void)
{
    s_page = (uint8_t)((s_page == 0u) ? 1u : 0u);
}

void display_toggle_hold(void)
{
    s_hold = (uint8_t)((s_hold == 0u) ? 1u : 0u);
}

void display_update(void)
{
    if (s_oled_ok == 0u) {
        return;/*屏幕没应答, 什么也不画*/
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
