/* Standalone LDC1614 ACK / ID test, not the measurement application. */
#include "test_board.h"
#include "test_i2c.h"

static void put_u32(uint32_t n)
{
    char digits[10];
    unsigned count = 0u;
    do { digits[count++] = (char)('0' + n % 10u); n /= 10u; } while (n != 0u);
    while (count != 0u) test_uart_putc(digits[--count]);
}

static void put_i32(int n)
{
    if (n < 0) { test_uart_putc('-'); put_u32((uint32_t)(-n)); }
    else put_u32((uint32_t)n);
}

static void put_hex(uint32_t n, unsigned digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits != 0u) {
        --digits;
        test_uart_putc(hex[(n >> (digits * 4u)) & 15u]);
    }
}

static void comma(void) { test_uart_putc(','); }

static void report_diag(const char *name, uint8_t address, int rc)
{
    test_i2c_diag_t d;
    test_i2c_get_diag(&d);
    test_uart_puts("$D,"); test_uart_puts(name); comma();
    put_hex(address, 2u); comma(); put_i32(rc); comma();
    test_uart_puts(test_i2c_stage_name(d.stage)); comma();
    put_hex(d.tx, 2u); comma(); put_u32(d.ack_level); comma();
    put_u32(d.lines); comma(); put_u32(d.clocks); comma();
    put_u32(d.hz); comma(); put_i32(d.stop_rc);
    test_uart_puts("\r\n");
}

static void test_address(uint8_t address)
{
    int probe_rc, mid_rc, did_rc;
    uint16_t mid = 0u, did = 0u;

    probe_rc = test_i2c_probe(address);
    report_diag("PROBE", address, probe_rc);
    /* Read even after a failed probe, to test a complete register-read path. */
    mid_rc = test_i2c_read_reg(address, 0x7Eu, &mid);
    report_diag("MID", address, mid_rc);
    did_rc = test_i2c_read_reg(address, 0x7Fu, &did);
    report_diag("DID", address, did_rc);

    test_uart_puts("$L,"); put_hex(address, 2u); comma();
    put_i32(probe_rc); comma(); put_i32(mid_rc); comma(); put_hex(mid, 4u);
    comma(); put_i32(did_rc); comma(); put_hex(did, 4u); comma();
    test_uart_puts(mid_rc == 0 && did_rc == 0 && mid == 0x5449u && did == 0x3055u
                   ? "LDC1614_FOUND\r\n" : "NOT_CONFIRMED\r\n");
}

int main(void)
{
    uint32_t round = 0u;
    int init_rc;

    test_board_init();
    test_uart_puts("\r\n$B,FW,C011_LDC_ACK_TEST_V1\r\n");
    test_uart_puts(test_board_uses_hse() ? "$B,CLOCK,HSE8M," : "$B,CLOCK,HSI_FALLBACK,");
    put_u32(test_board_clock_hz()); test_uart_puts("\r\n");
    test_uart_puts("$B,PINS,PB7_SCL,PB6_SDA,PA2_TX,PA3_RX,PA4_INPUT\r\n");
    test_uart_puts("$B,MODE,SDA_INPUT_ACK,HALF_DELAY_US,5,NO_LDC_CONFIG_WRITES\r\n");
    test_delay_ms(100u);
    init_rc = test_i2c_init();
    report_diag("BUS_INIT", 0u, init_rc);

    for (;;) {
        test_uart_puts("$B,ROUND,"); put_u32(++round); comma();
        put_u32(test_bus_lines()); comma(); put_u32((uint32_t)test_intb_read());
        test_uart_puts("\r\n");
        test_address(0x2Au);
        test_address(0x2Bu);
        test_uart_puts("$B,ROUND_END\r\n");
        test_delay_ms(1000u);
    }
}
