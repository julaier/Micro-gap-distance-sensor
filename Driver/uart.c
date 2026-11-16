/*
 * uart.c - STM32C011 USART2 查询收发
 *
 * PA2 = USART2_TX (AF1) -> CH340 RXD
 * PA3 = USART2_RX (AF1) <- CH340 TXD
 */

#include <string.h>
#include "stm32c0xx.h"
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_gpio.h"
#include "stm32c0xx_ll_rcc.h"
#include "stm32c0xx_ll_usart.h"
#include "uart.h"

void uart_init(uint32_t baud)
{
    LL_GPIO_InitTypeDef gpio = {0};
    LL_USART_InitTypeDef usart = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
    gpio.Pin = LL_GPIO_PIN_2 | LL_GPIO_PIN_3;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_UP;
    gpio.Alternate = LL_GPIO_AF_1;
    LL_GPIO_Init(GPIOA, &gpio);

    usart.PrescalerValue = LL_USART_PRESCALER_DIV1;
    usart.BaudRate = baud;
    usart.DataWidth = LL_USART_DATAWIDTH_8B;
    usart.StopBits = LL_USART_STOPBITS_1;
    usart.Parity = LL_USART_PARITY_NONE;
    usart.TransferDirection = LL_USART_DIRECTION_TX_RX;
    usart.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    usart.OverSampling = LL_USART_OVERSAMPLING_16;
    LL_USART_Init(USART2, &usart);
    LL_USART_DisableFIFO(USART2);
    LL_USART_ConfigAsyncMode(USART2);
    LL_USART_Enable(USART2);

    while ((LL_USART_IsActiveFlag_TEACK(USART2) == 0u) ||
           (LL_USART_IsActiveFlag_REACK(USART2) == 0u)) {
    }
}

void uart_write(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0u; i < len; i++) {
        while (LL_USART_IsActiveFlag_TXE(USART2) == 0u) {
        }
        LL_USART_TransmitData8(USART2, buf[i]);
    }

    while (LL_USART_IsActiveFlag_TC(USART2) == 0u) {
    }
}

void uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    uart_write((const uint8_t *)s, (uint16_t)strlen(s));
}

int uart_read_byte(uint8_t *b)
{
    if (LL_USART_IsActiveFlag_RXNE(USART2) == 0u) {
        return 0;
    }
    if (b != 0) {
        *b = LL_USART_ReceiveData8(USART2);
    } else {
        (void)LL_USART_ReceiveData8(USART2);
    }
    return 1;
}
