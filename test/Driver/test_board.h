#ifndef TEST_BOARD_H
#define TEST_BOARD_H

#include <stdint.h>

/* Only clock, USART2, TIM14 and the two GPIO lines. No IRQ or old drivers. */
void test_board_init(void);
int test_board_uses_hse(void);
uint32_t test_board_clock_hz(void);
uint16_t test_time_us(void);
void test_delay_us(uint16_t us);
void test_delay_ms(uint16_t ms);
void test_uart_putc(char c);
void test_uart_puts(const char *s);

/* 1 releases an open-drain line; 0 pulls it low. PB7=SCL, PB6=SDA. */
void test_scl_write(int release);
void test_sda_write(int release);
int test_scl_read(void);
int test_sda_read(void);
void test_sda_input(int input);
uint8_t test_bus_lines(void); /* bit0=SCL, bit1=SDA */
int test_intb_read(void);     /* PA4 input only; not used for I2C decisions */

#endif
