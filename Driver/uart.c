/*
 * uart.c - USART2 发送实现 (查询发送)
 *
 * PA2 = USART2_TX -> CH340 的 RXD
 * 当前只发送, 电脑不能发命令给单片机。
 * 本文件使用 STM32F10x Standard Peripheral Library USART/GPIO/RCC API。
 */

#include <string.h>
#include "stm32f10x.h"
#include "uart.h"

//baud = 115200, 8N1, PA2 = TX
void uart_init(uint32_t baud)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    /* PA2 = 复用推挽输出 */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_2;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = baud;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &usart);
    USART_Cmd(USART2, ENABLE);
}

//发送数据，等待发送完成
void uart_write(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++) {
        // 等待发送缓冲空
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART2, buf[i]);
    }

    // 等待最后一个字节真正发送完成
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET) {
    }
}

//发送字符串，遇到 '\0' 结束
void uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    uart_write((const uint8_t *)s, (uint16_t)strlen(s));
}
