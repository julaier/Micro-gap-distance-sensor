/*
 * board.h - 板级基础服务: 系统时钟、毫秒时基、LDC1614 INTB 中断
 *
 * 适用板: STM32F103C8T6 + LDC1614RGHR (见 ../README.md)
 * 本文件不包含任何测量/协议逻辑, 只提供最底层的能力。
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* 上电初始化: 72MHz 时钟 -> 1ms 时基 -> INTB 中断。
 * 必须在所有其它模块之前调用。 */
void board_init(void);

/* 毫秒计数, 1ms 递增, 溢出后回绕 (比较时用无符号相减) */
uint32_t board_millis(void);

void board_delay_ms(uint32_t ms);
void board_delay_us(uint32_t us);

/* 系统时钟当前是不是 72MHz; 0 表示退到了内部 HSI 8MHz */
int board_clock_is_72m(void);

/* LDC1614 的 INTB 接 PB0, 下降沿中断。
 * board_intb_take() 取走标志, 返回 1 表示取走后需要读一次转换结果。 */
int board_intb_take(void);

void board_feed_watchdog(void);
void board_reset(void);

#endif /* BOARD_H */
