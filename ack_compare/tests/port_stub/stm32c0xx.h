#ifndef ACK_PORT_TEST_STM32_H
#define ACK_PORT_TEST_STM32_H
#include <stdint.h>
typedef struct { volatile uint32_t IDR, BSRR; } ack_gpio_stub_t;
extern ack_gpio_stub_t ack_gpio_b;
#define GPIOB (&ack_gpio_b)
#endif
