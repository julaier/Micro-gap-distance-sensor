/*
 * uart.h - USART2 串口 (接外部 CH340 模块)
 *
 * PA2 = USART2_TX -> CH340 的 RXD
 * 当前只发送, 电脑不能发命令给单片机。
 * 3.3V TTL, 默认 115200 8N1。
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
//发送数据，等待发送完成
void uart_write(const uint8_t *buf, uint16_t len);
//发送字符串，遇到 '\0' 结束
void uart_puts(const char *s);

#endif /* UART_H */
