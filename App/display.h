/*
 * display.h - OLED 显示层
 *
 * 只负责"把测量结果画到屏幕上", 不参与采样和状态机。
 * 主界面: 距离(大字) + 温度 + 状态 + 比值
 * 调试界面: CH0 / CH1 / 比值 / 温度 / 状态
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* 上电初始化: 返回 0 = OLED 正常, -1 = 屏幕没应答 */
int  display_init(void);

/* 刷新显示: 内部从 measure_latest() 取数据 */
void display_update(void);

/* KEY1: 主界面 <-> 调试界面 */
void display_toggle_page(void);

/* KEY2: 冻结 / 恢复显示 */
void display_toggle_hold(void);

#endif /* DISPLAY_H */
