/*
 * console.h - 串口命令接收 (USART1 RX)
 *
 * 把收到的字符拼成一行, 交给 protocol_handle_line() 解析,
 * 再把回复从串口发出去。命令以 \r 或 \n 结尾。
 */
#ifndef CONSOLE_H
#define CONSOLE_H

void console_init(void);
void console_poll(void);

#endif /* CONSOLE_H */
