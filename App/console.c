/*
 * console.c - 串口命令接收
 *
 * 每轮主循环调用 console_poll():
 *   收字符 -> 拼成一行 -> protocol_handle_line() -> 回复
 */

#include "console.h"
#include "uart.h"
#include "protocol.h"

#define CONSOLE_LINE_MAX  96

static char s_line[CONSOLE_LINE_MAX];
static int  s_len;

void console_init(void)
{
    s_len = 0;
    s_line[0] = '\0';
}

void console_poll(void)
{
    uint8_t ch;
    char reply[192];
    int n;

    while (uart_read_byte(&ch) != 0) {
        if (ch == '\r' || ch == '\n') {
            if (s_len == 0) {
                continue;/* 空的回车忽略 */
            }

            s_line[s_len] = '\0';
            n = protocol_handle_line(s_line, reply, (int)sizeof(reply));
            if (n > 0) {
                uart_write((const uint8_t *)reply, (uint16_t)n);
            }
            s_len = 0;
        } else if (ch == 0x08u || ch == 0x7Fu) {
            /* 退格 */
            if (s_len > 0) {
                s_len--;
            }
        } else if (s_len < (CONSOLE_LINE_MAX - 1)) {
            s_line[s_len++] = (char)ch;
        } else {
            s_len = 0;/* 一行太长, 丢掉 */
        }
    }
}
