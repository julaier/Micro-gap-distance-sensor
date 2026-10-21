/*
 * button.h - PA0 / PA1 两个按键 (按下为低电平)
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

#define BTN_KEY1   0   /* PA0 */
#define BTN_KEY2   1   /* PA1 */

void button_init(void);

/* 主循环里反复调用, 内部做 20ms 消抖 */
void button_scan(uint32_t now_ms);

/* 短按事件: 有事件返回 1, 读一次就清掉 */
int  button_short_press(int idx);

/* 长按事件(按住 2 秒): 有事件返回 1, 读一次就清掉 */
int  button_long_press(int idx);

#endif /* BUTTON_H */
