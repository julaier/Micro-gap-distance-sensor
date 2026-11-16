/*
 * board.h - STM32C011F6U6TR 板级基础服务
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

void board_init(void);
/* 在 test 最小环境读 ID 完成后，启动正式应用的 SysTick 和 INTB 中断。 */
void board_start_app_services(void);
uint32_t board_millis(void);
void board_delay_ms(uint32_t ms);
void board_delay_us(uint32_t us);

/* HSE 8 MHz 为正常目标配置。晶振失败时系统退回 HSI 48 MHz。 */
int board_clock_is_hse8(void);

/* 保留旧接口兼容性；STM32C011 无 PLL，本工程正常返回 0。 */
int board_clock_is_72m(void);

/* LDC1614 INTB 接 PA4，下降沿中断。 */
int board_intb_take(void);

void board_feed_watchdog(void);
void board_reset(void);

#endif /* BOARD_H */
