/*
 * oled.c - SSD1306 128x64 OLED 驱动 (I2C)
 *
 * 型号: HS96L03W2C03 (0.96 寸, SSD1306 兼容, 128x64)
 * 和 LDC1614 共用 I2C1: SCL = PB6, SDA = PB7
 *
 * 显存取 1KB 内存, 画完之后 oled_flush() 一次刷到屏幕。
 */

#include <string.h>
#include "stm32f10x.h"
#include "oled.h"
#include "hard_i2c.h"

#define OLED_CMD   0x00u   /* 控制字节: 后面跟的是命令 */
#define OLED_DAT   0x40u   /* 控制字节: 后面跟的是显存数据 */

static uint8_t s_fb[OLED_WIDTH * OLED_HEIGHT / 8u];

/* 5x7 点阵: 每个字符 5 列, 每列 bit0..bit6 从上到下 */
static const uint8_t FONT5X7[][5] = {
    /* 0-9 */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    /* A-Z */
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
    /* 空格, '.', ':', '-', '/', '?' */
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x60,0x60,0x00,0x00},
    {0x00,0x36,0x36,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x20,0x10,0x08,0x04,0x02}, {0x02,0x01,0x51,0x09,0x06}
};

static const uint8_t *font_of(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= '0' && c <= '9') {
        return FONT5X7[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return FONT5X7[10 + (c - 'A')];
    }
    switch (c) {
    case ' ': return FONT5X7[36];
    case '.': return FONT5X7[37];
    case ':': return FONT5X7[38];
    case '-': return FONT5X7[39];
    case '/': return FONT5X7[40];
    default:  return FONT5X7[41];
    }
}

/* 发一串命令: 前面加 0x00 控制字节 */
static int write_cmds(const uint8_t *cmds, uint16_t n)
{
    uint8_t buf[32];
    uint16_t i;

    if (n > 31u) {
        n = 31u;
    }
    buf[0] = OLED_CMD;
    for (i = 0; i < n; i++) {
        buf[i + 1u] = cmds[i];
    }
    return hard_i2c_write(OLED_I2C_ADDR, buf, (uint16_t)(n + 1u));
}

int oled_init(void)
{
    static const uint8_t init[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex 64 */
        0xD3, 0x00, /* display offset 0 */
        0x40,       /* start line 0 */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x02, /* page addressing mode */
        0xA1,       /* segment remap */
        0xC8,       /* COM scan direction */
        0xDA, 0x12, /* COM pins */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* precharge */
        0xDB, 0x40, /* VCOMH */
        0xA4,       /* display from RAM */
        0xA6,       /* normal display */
        0xAF        /* display on */
    };

    if (write_cmds(init, (uint16_t)sizeof(init)) != 0) {
        return -1;/*屏幕没应答: 地址不对或者没接*/
    }

    oled_clear();
    oled_flush();
    return 0;
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void set_pixel(int x, int y, uint8_t on)
{
    uint16_t idx;

    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    idx = (uint16_t)((y / 8) * OLED_WIDTH + x);
    if (on != 0u) {
        s_fb[idx] |= (uint8_t)(1u << (y & 7));
    } else {
        s_fb[idx] &= (uint8_t)~(1u << (y & 7));
    }
}

void oled_draw_char(int x, int y, char c, uint8_t scale)
{
    const uint8_t *g = font_of(c);
    int col;
    int row;

    if (scale == 0u) {
        scale = 1u;
    }

    for (col = 0; col < 5; col++) {
        uint8_t bits = g[col];

        for (row = 0; row < 7; row++) {
            if ((bits & (uint8_t)(1u << row)) != 0u) {
                int px;
                int py;

                for (py = 0; py < (int)scale; py++) {
                    for (px = 0; px < (int)scale; px++) {
                        set_pixel(x + col * (int)scale + px,
                                  y + row * (int)scale + py, 1u);
                    }
                }
            }
        }
    }
}

void oled_draw_str(int x, int y, const char *s, uint8_t scale)
{
    if (s == 0) {
        return;
    }
    if (scale == 0u) {
        scale = 1u;
    }

    while (*s != '\0') {
        oled_draw_char(x, y, *s, scale);
        x += 6 * (int)scale;/*5 列点阵 + 1 列间隔*/
        s++;
    }
}

int oled_text_width(const char *s, uint8_t scale)
{
    int n = 0;

    if (s == 0) {
        return 0;
    }
    if (scale == 0u) {
        scale = 1u;
    }
    while (*s != '\0') {
        n++;
        s++;
    }
    return n * 6 * (int)scale;
}

void oled_draw_str_right(int right, int y, const char *s, uint8_t scale)
{
    oled_draw_str(right - oled_text_width(s, scale), y, s, scale);
}

void oled_flush(void)
{
    uint8_t buf[OLED_WIDTH + 1u];
    uint8_t page;
    uint16_t i;

    for (page = 0; page < (OLED_HEIGHT / 8u); page++) {
        uint8_t cmd[3];

        cmd[0] = (uint8_t)(0xB0u | page);/*页地址*/
        cmd[1] = 0x00u;                  /*列地址低 4 位*/
        cmd[2] = 0x10u;                  /*列地址高 4 位*/
        if (write_cmds(cmd, 3u) != 0) {
            return;
        }

        buf[0] = OLED_DAT;
        for (i = 0; i < OLED_WIDTH; i++) {
            buf[i + 1u] = s_fb[(uint16_t)page * OLED_WIDTH + i];
        }
        if (hard_i2c_write(OLED_I2C_ADDR, buf, (uint16_t)(OLED_WIDTH + 1u)) != 0) {
            return;
        }
    }
}
