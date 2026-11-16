/*
 * uart.h - STM32C011 USART2
 *
 * PA2 = USART2_TX -> CH340 RXD
 * PA3 = USART2_RX <- CH340 TXD
 * 3.3 V TTL，默认 115200 8N1。
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
void uart_write(const uint8_t *buf, uint16_t len);
void uart_puts(const char *s);
int uart_read_byte(uint8_t *b);

#endif /* UART_H */
