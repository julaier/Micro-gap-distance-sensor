#include "test_board.h"
#include "f103_flow.h"
#include "stm32c0xx.h"
#ifdef MARGIN_I2C
#include "margin_i2c.h"
#endif

static void number(uint32_t v)
{
    char b[10]; unsigned n = 0u;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) test_uart_putc(b[--n]);
}
int main(void)
{
    uint32_t round = 0u;
    unsigned polls = 0u;
    int rc;
    g_flow_debug.mcu_reset_flags = RCC->CSR2;
    RCC->CSR2 |= RCC_CSR2_RMVF;
    test_board_init();
#ifdef MARGIN_I2C
    test_uart_puts("\r\n$B,FW,C011_LDC_F103_FLOW_V9_MARGIN\r\n");
    test_uart_puts("$B,PROFILE,TIM14_MARGIN_OD\r\n");
    test_uart_puts("$B,TIMING_US,LOW_MIN,20,HIGH_MIN,18,SETUP_MIN,8,HOLD_MIN,2,PLUS_1TICK\r\n");
#else
    test_uart_puts("\r\n$B,FW,C011_LDC_F103_FLOW_V8_SW\r\n");
    test_uart_puts("$B,PROFILE,B_OD3_UNCHANGED\r\n");
#endif
    test_uart_puts(test_board_uses_hse() ? "$B,CLOCK,HSE8M," : "$B,CLOCK,HSI_FALLBACK,");
    number(test_board_clock_hz());
    test_uart_puts("\r\n$B,PINS,PB7_SCL,PB6_SDA,PA2_TX,PA3_RX,PA4_INPUT\r\n");
    test_uart_puts("$B,CONFIG,F103_ORIGINAL_FLOW,EXT40,NO_EXTRA_SLEEP_WRITE\r\n");
    test_uart_puts("$B,MODE,NO_IRQ,NO_ADC,NO_LDC_REF_FALLBACK,NO_RETRY\r\n");
    test_delay_ms(100u);
    rc = f103_flow_start();
#ifdef MARGIN_I2C
    margin_i2c_report();
#endif
    for (;;) {
        if (rc == 0) {
            test_delay_ms(20u); /* Polling interval + I2C/UART execution overhead. */
            f103_flow_poll();
            if (++polls >= 50u) { polls = 0u; f103_flow_monitor(++round); }
        } else {
            test_delay_ms(1000u);
            f103_flow_monitor(++round);
        }
    }
}
