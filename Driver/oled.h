/*
 * oled.h - 0.96 寸 OLED (SSD1306, 128x64, I2C) 驱动
 *
 * 型号: HS96L03W2C03
 * 接法: 和 LDC1614 共用 I2C1 (SCL = PB6, SDA = PB7)
 * 7 位地址默认 0x3C (模块背面电阻可改 0x3D)
 */
#ifndef OLED_H
#define OLED_H

#include <stdint.h>

#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_I2C_ADDR   0x3Cu

/* 初始化: 返回 0 成功, -1 表示屏幕不应答 (地址不对或没接) */
int  oled_init(void);

void oled_clear(void);
void oled_flush(void);          /* 把显存刷到屏幕 */

/* 画字符/字符串, scale = 字号倍数 (1 = 6x8, 2 = 12x16, 3 = 18x24) */
void oled_draw_char(int x, int y, char c, uint8_t scale);
void oled_draw_str(int x, int y, const char *s, uint8_t scale);
void oled_draw_str_right(int right, int y, const char *s, uint8_t scale);

/* 字符串占用的像素宽度 */
int  oled_text_width(const char *s, uint8_t scale);

#endif /* OLED_H */
