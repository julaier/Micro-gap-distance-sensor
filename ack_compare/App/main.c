/* Independent wake/configuration test. No application services or IRQ setup. */
#include "test_board.h"
#include "wake_diag.h"
#include "stm32c0xx.h"

static void put_u32(uint32_t value)
{
    char digits[10];
    unsigned n = 0u;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) test_uart_putc(digits[--n]);
}

static void put_hex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    unsigned n = 8u;
    while (n) { --n; test_uart_putc(digits[(value >> (n * 4u)) & 15u]); }
}

int main(void)
{
    uint32_t round = 0u;

    /* STM32 reset evidence only: this cannot identify an LDC reset cause.
     * Capture/clear flags once so a later boot does not accumulate old flags. */
    g_wake_debug.mcu_reset_flags = RCC->CSR2;
    RCC->CSR2 |= RCC_CSR2_RMVF;
    test_board_init(); /* Original test: SysTick off, PA4 input, TIM14 polling. */
#if ACK_COMPARE_PROFILE == 2
    test_uart_puts("\r\n$B,FW,C011_LDC_ACK_EDGE_V7_F103\r\n");
    test_uart_puts("$B,EARLY_IDR,PB7_SCL,PB6_SDA,8_READS,NO_FIXED_INTERVAL\r\n");
#else
    test_uart_puts("\r\n$B,FW,C011_LDC_ACK_COMPARE_V6_F103\r\n");
#endif
    test_uart_puts("$B,PROFILE," ACK_PROFILE_NAME "\r\n");
    test_uart_puts("$B,MCU_RESET_CSR2,");
    put_hex32(g_wake_debug.mcu_reset_flags);
    test_uart_puts("\r\n");
    test_uart_puts(test_board_uses_hse() ? "$B,CLOCK,HSE8M," : "$B,CLOCK,HSI_FALLBACK,");
    put_u32(test_board_clock_hz());
    test_uart_puts("\r\n$B,PINS,PB7_SCL,PB6_SDA,PA2_TX,PA3_RX,PA4_INPUT\r\n");
    test_uart_puts("$B,MODE,SLEEP_HOLD_1S,F103_AUTO_DRIVE,EXT40,NO_IRQ,NO_ADC,NO_FALLBACK\r\n");
    test_uart_puts("$B,CONFIG,F103_ORIGINAL,SLEEP,2A01,WAKE,0A01\r\n");
    test_delay_ms(100u);
    (void)wake_diag_start();

    for (;;) {
        test_delay_ms(1000u);
        wake_diag_monitor(++round);
    }
}
