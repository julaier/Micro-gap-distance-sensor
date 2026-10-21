/*
 * uart.c - USART1 收发实现 (查询发送)
 *
 * PA9  = USART1_TX -> CH340 的 RXD
 * PA10 = USART1_RX <- CH340 的 TXD
 * 本文件使用 STM32F10x Standard Peripheral Library USART/GPIO/RCC API。
 */

#include <string.h>
#include "stm32f10x.h"
#include "uart.h"

//baud = 115200, 8N1, PA9 = TX / PA10 = RX
void uart_init(uint32_t baud)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

    /* PA9 = 复用推挽输出 (TX) */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_9;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PA10 = 浮空输入 (RX) */
    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = baud;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
}

//发送数据，等待发送完成
void uart_write(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++) {
        // 等待发送缓冲空
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, buf[i]);
    }

    // 等待最后一个字节真正发送完成
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {
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

//非阻塞读一个字节: 读到返回 1, 没数据返回 0
int uart_read_byte(uint8_t *b)
{
    if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == RESET) {
        return 0;
    }
    if (b != 0) {
        *b = (uint8_t)USART_ReceiveData(USART1);
    } else {
        (void)USART_ReceiveData(USART1);
    }
    return 1;
}
