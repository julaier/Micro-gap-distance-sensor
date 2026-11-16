/* Real app + I2C + LDC + old measurement/protocol/console on virtual hardware.
 * Reuse the same edge-decoding wires; retain the driver's regression main
 * under another name so that its static checks remain compilable too. */
#define main i2c_regression_main
#include "test_ported_i2c.c"
#undef main
#include "../../App/app.c"
#include "flash_param.h"

static char serial_output[32768];
static size_t serial_length;
static const char *serial_input;
static unsigned requested_fault, requested_fault_number, intb_event, flash_saves;
static unsigned missing_external, missing_internal, requested_limit = 0xFFFFFFFFu;
static unsigned zero_sensor_data;
static uint8_t flash_bytes[2048];

void board_init(void)
{
    reset_model(0x2Au);
    config_fault = requested_fault;
    fault_config_number = requested_fault_number;
    conversions_enabled = 1u;
    external_ref_running = !missing_external;
    internal_ref_running = !missing_internal;
    conversion_limit = requested_limit;
    if (zero_sensor_data) conversion_value0 = conversion_value1 = 0u;
    serial_length = 0u;
    serial_output[0] = '\0';
    serial_input = "";
    intb_event = 0u;
}
void board_start_app_services(void) { }
int board_clock_is_hse8(void) { return 1; }
uint32_t board_millis(void) { return ticks / 1000u; }
void test_delay_ms(uint16_t ms) { ticks += (uint32_t)ms * 1000u; }
int board_intb_take(void) { int value = (int)intb_event; intb_event = 0u; return value; }
void board_feed_watchdog(void) { }
void board_reset(void) { assert(!"Unexpected reset command"); }
void uart_init(uint32_t baud) { (void)baud; assert(!"Keep the proven test UART initialization"); }
void uart_write(const uint8_t *buf, uint16_t len)
{
    assert(serial_length + len < sizeof(serial_output));
    memcpy(serial_output + serial_length, buf, len);
    serial_length += len;
    serial_output[serial_length] = '\0';
}
void uart_puts(const char *s) { uart_write((const uint8_t *)s, (uint16_t)strlen(s)); }
int uart_read_byte(uint8_t *b)
{
    if (*serial_input == '\0') return 0;
    *b = (uint8_t)*serial_input++;
    return 1;
}
void button_init(void) { }
void button_scan(uint32_t now) { (void)now; }
int button_short_press(int key) { (void)key; return 0; }
void adc_temp_init(void) { }
int32_t adc_temp_read_x10(void) { return 250; }
void adc_temp_debug(uint16_t *cal, uint16_t *vref, uint16_t *ts) { *cal = *vref = *ts = 0u; }
const void *flash_param_ptr(void) { return flash_bytes; }
int flash_param_write(const void *src, uint32_t len)
{
    assert(len <= sizeof(flash_bytes));
    memcpy(flash_bytes, src, len);
    ++flash_saves;
    return 0;
}

static void set_data(uint32_t ch0, uint32_t ch1)
{
    conversion_value0 = ch0;
    conversion_value1 = ch1;
    registers[0] = (uint16_t)(ch0 >> 16);
    registers[1] = (uint16_t)ch0;
    registers[2] = (uint16_t)(ch1 >> 16);
    registers[3] = (uint16_t)ch1;
    registers[0x18] |= 0x004Cu;
}
static void poll_after(uint32_t us)
{
    /* Bounds apply to one captured operation, not an unbounded simulation. */
    trace_count = address_count = master_ack_count = 0u;
    ticks += us;
    app_poll();
}
static void settle(uint32_t ch0, uint32_t ch1)
{
    set_data(ch0, ch1);
    for (unsigned i = 0u; i < 160u; ++i) poll_after(20000u);
}
static void command(const char *line)
{
    serial_input = line;
    poll_after(0u);
    assert(*serial_input == '\0');
}

int main(void)
{
    const measure_result_t *r;
    float alarm, warn, hyst, k, tref;
    uint32_t last_success;
    requested_fault = requested_fault_number = 0u;
    app_init();
    assert(s_ldc_ready && measure_cal_count() == 9);
    assert(strstr(serial_output, "$C,1A,2A01,0,0,2A01,OK\r\n") != NULL);
    assert(strstr(serial_output, "$C,1A,0A01,0,0,0A01,OK\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC1614 ready\r\n") != NULL);
    assert(strstr(serial_output, "$V,EXT40,2,0,004C,120000,100000,0000\r\n") != NULL);
    assert(strstr(serial_output, "LDC_FALLBACK") == NULL);
    assert(strstr(serial_output, "$K,BOOT,COMM,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,INIT,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,HOLD,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,DATA,0\r\n") != NULL);

    /* Every stable setting, including CH1, is checked without any write.
     * Automatic DRIVE_CURRENT changes are deliberately not compared. */
    {
        const unsigned regs[] = {0x1A, 0x1B, 8, 9, 0x10, 0x11, 0x19, 0x14, 0x15};
        unsigned writes = register_writes;
        for (unsigned i = 0u; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            uint16_t saved = registers[regs[i]];
            registers[regs[i]] ^= 1u;
            assert(check_ldc_config("EXT40", 1, 0) == LDC1614_ERR_CONFIG_HOLD);
            registers[regs[i]] = saved;
        }
        registers[0x1E] = 0x9040u;
        assert(check_ldc_config("EXT40", 1, 0) == 0 && register_writes == writes);
    }
    settle(120000u, 100000u);
    r = measure_latest();
    assert(r->ch0 == 120000u && r->ch1 == 100000u && r->valid && r->status == MEAS_ST_NORMAL);
    assert(r->ratio > 1.19999f && r->ratio < 1.20001f);
    assert(r->dist_mm > 0.231f && r->dist_mm < 0.233f && r->temp_c == 25.0f);
    assert(strstr(serial_output, "$R,120000,100000,1.20000,0.232,25.0,0\r\n") != NULL);

    settle(110000u, 100000u);
    assert(measure_latest()->status == MEAS_ST_WARN);
    settle(103229u, 100000u);
    assert(measure_latest()->status == MEAS_ST_ALARM);
    settle(120000u, 100000u);
    assert(measure_latest()->status == MEAS_ST_NORMAL);

    /* INTB path also works before the 20ms polling fallback is due. */
    set_data(121000u, 100000u);
    intb_event = 1u;
    poll_after(1000u);
    assert(measure_latest()->ch0 == 121000u && intb_event == 0u);

    command("RAW\rREAD\rINFO\rRD,7E\rRATE,250\rWR,08,1234\rCALS\r");
    assert(strstr(serial_output, "INFO,v1.0,rate=100,N=9") != NULL);
    assert(strstr(serial_output, "REG,126,21577\r\n") != NULL);
    assert(protocol_get_rate_ms() == 250u && registers[8] == 0x1234u && flash_saves == 1u);
    command("STX,1.25\rST2,0.75\rTC,0.00001,25\rSWAP,1\r");
    measure_get_thresholds(&alarm, &warn, &hyst);
    measure_get_temp_comp(&k, &tref);
    assert(alarm == 1.25f && warn == 0.75f && hyst == 0.05f && measure_get_swap() == 1u);
    assert(k > 0.000009f && k < 0.000011f && tref == 25.0f);

    last_success = s_last_data_ms;
    force_sda_low = 1;
    for (unsigned i = 0u; i < 30u; ++i) poll_after(20000u);
    assert(measure_latest()->status == MEAS_ST_FAULT && !measure_latest()->valid);
    assert(s_last_data_ms == last_success); /* Failed polls never refresh it. */
    force_sda_low = 0;
    command("SWAP,0\rTC,0,0\r");
    settle(120000u, 100000u);
    assert(measure_latest()->valid && measure_latest()->status == MEAS_ST_NORMAL);

    /* CONFIG NACK is not hidden: it is logged and read back exactly. */
    requested_fault = 2u;
    requested_fault_number = 2u;
    app_init();
    assert(s_ldc_ready);
    assert(strstr(serial_output, "$C,1A,0A01,-2,0,0A01,NACK_VERIFIED\r\n") != NULL);

    /* Reproduce user's NACK + CONFIG=2801; snapshot precedes the reset/retry. */
    requested_fault = 7u;
    app_init();
    assert(s_ldc_ready && registers[0x1A] == 0x0801u);
    assert(strstr(serial_output, "$C,1A,0A01,-2,0,2801,FAIL\r\n") != NULL);
    assert(strstr(serial_output, "$X,INIT,-2,WRITE,2,2\r\n") != NULL);
    assert(strstr(serial_output, "$Q,EXT40,1B,0,020F\r\n") != NULL);
    assert(strstr(serial_output, "$Q,EXT40,08,0,0080\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC_CLOCK,INTERNAL,RECHECK_CAL\r\n") != NULL);
    assert(strstr(serial_output, "$C,1A,0801,0,0,0801,OK\r\n") != NULL);
    assert(strstr(serial_output, "$V,INTERNAL,2,0,004C,120000,100000,0000\r\n") != NULL);
    assert(strstr(serial_output, "$E,") == NULL);
    settle(120000u, 100000u);
    assert(measure_latest()->valid && measure_latest()->status == MEAS_ST_NORMAL);

    /* Same CONFIG image alone cannot distinguish reset from CONFIG-only loss. */
    requested_fault = 9u;
    app_init();
    assert(s_ldc_ready);
    assert(strstr(serial_output, "$Q,EXT40,1A,0,2801\r\n") != NULL);
    assert(strstr(serial_output, "$Q,EXT40,1B,0,820C\r\n") != NULL);
    assert(strstr(serial_output, "$Q,EXT40,08,0,FFFF\r\n") != NULL);

    requested_fault = 8u;
    app_init();
    assert(!s_ldc_ready && measure_latest()->status == MEAS_ST_FAULT);
    assert(strstr(serial_output, "$E,17\r\n") != NULL);
    set_data(120000u, 100000u);
    {
    unsigned writes = register_writes;
    for (unsigned i = 0u; i < 10u; ++i) {
        poll_after(100000u);
        if (i < 9u) assert(address_count == 0u);
        else assert(address_count != 0u); /* 1-second read-only diagnostics. */
        assert(register_writes == writes && measure_latest()->status == MEAS_ST_FAULT);
    }
    }

    /* Precisely reproduce reset images on BOTH external and internal wake.
     * COMM remains good, INIT/HOLD fail, DATA is skipped, never ready. */
    requested_fault = 10u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,17\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,INIT,-17\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,INIT,-17\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,HOLD,-18\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,DATA,-4\r\n") != NULL);
    assert(strstr(serial_output, "$Q,INTERNAL,1B,0,020F\r\n") != NULL);
    assert(strstr(serial_output, "$Q,INTERNAL,08,0,0080\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC1614 ready") == NULL);
    {
        unsigned writes = register_writes, wake_writes = config_writes;
        poll_after(1000000u);
        assert(register_writes == writes && config_writes == wake_writes);
        assert(strstr(serial_output, "$D,DIAG_ID,0,5449,0,3055\r\n") != NULL);
        assert(strstr(serial_output, "$K,INTERNAL,COMM,0\r\n") != NULL);
        slave_address = 0x2Bu; /* Disconnect/lose the target during monitoring. */
        poll_after(1000000u);
        assert(strstr(serial_output, "$K,INTERNAL,COMM,-11\r\n") != NULL);
        assert(strstr(serial_output, "$Q,INTERNAL,1A,-2,0000\r\n") != NULL);
        assert(strstr(serial_output, "$K,INTERNAL,HOLD,-2\r\n") != NULL);
        assert(register_writes == writes && !s_ldc_ready);
    }

    /* CONFIG alone matches after wake, but another setting is lost. Even
     * fresh nonzero conversions cannot turn that into a successful startup. */
    requested_fault = 11u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,18\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,INIT,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,HOLD,-18\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,DATA,0\r\n") != NULL);
    assert(strstr(serial_output, "$Q,EXT40,08,0,0080\r\n") != NULL);

    /* A STATUS read NACK is a real communication error, not DATA=1/E20. */
    requested_fault = 12u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,2\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,DATA,-2\r\n") != NULL);
    assert(strstr(serial_output, "$K,EXT40,HOLD,0\r\n") != NULL);
    assert(strstr(serial_output, "LDC_FALLBACK") == NULL);
    assert(strstr(serial_output, "$E,20\r\n") == NULL);

    /* Config ACKs/readback alone are insufficient: compare missing reference. */
    requested_fault = 0u;
    missing_external = 1u;
    app_init();
    assert(s_ldc_ready && registers[0x1A] == 0x0801u);
    assert(strstr(serial_output, "$B,LDC_ATTEMPT,EXT40,-20\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC_CLOCK,INTERNAL,RECHECK_CAL\r\n") != NULL);
    assert(strstr(serial_output, "$E,") == NULL);
    missing_internal = 1u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,20\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,INIT,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,HOLD,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,DATA,1\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC_DIAG_ONLY\r\n") != NULL);
    {
        unsigned writes = register_writes;
        poll_after(1000000u);
        assert(register_writes == writes && !s_ldc_ready);
        assert(strstr(serial_output, "$Q,INTERNAL,18,0,0000\r\n") != NULL);
        assert(strstr(serial_output, "$Q,INTERNAL,00,0,0000\r\n") != NULL);
    }

    /* Synthetic missing/non-oscillating sensor: conversions carry zeros.
     * Distinguish it from bus/config failure, without claiming a physical cause. */
    missing_external = missing_internal = 0u;
    zero_sensor_data = 1u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,20\r\n") != NULL);
    assert(strstr(serial_output, "$K,BOOT,COMM,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,HOLD,0\r\n") != NULL);
    assert(strstr(serial_output, "$K,INTERNAL,DATA,1\r\n") != NULL);
    assert(strstr(serial_output, "$B,LDC1614 ready") == NULL);
    last_success = s_last_data_ms;
    {
        unsigned writes = register_writes;
        for (unsigned i = 0u; i < 25u; ++i) poll_after(100000u);
        assert(register_writes == writes && s_last_data_ms == last_success);
        assert(!measure_latest()->valid && measure_latest()->status == MEAS_ST_FAULT);
    }
    zero_sensor_data = 0u;

    /* One stale completed pair is not continuous conversion proof. */
    missing_external = missing_internal = 0u;
    requested_limit = 1u;
    app_init();
    assert(!s_ldc_ready && strstr(serial_output, "$E,20\r\n") != NULL);
    requested_limit = 0xFFFFFFFFu;
    app_init();
    assert(s_ldc_ready);
    settle(120000u, 100000u);
    last_success = s_last_data_ms;
    conversions_enabled = 0u;
    for (unsigned i = 0u; i < 30u; ++i) poll_after(20000u);
    assert(s_last_data_ms == last_success);
    assert(measure_latest()->status == MEAS_ST_FAULT);
    puts("PASS: real app/console/I2C/LDC/old calibration/protocol pipeline and serial frames.");
    puts("PASS: NORMAL/WARN/ALARM, INTB/20ms polling, commands and fault/recovery.");
    puts("PASS: wake reset snapshots, one-shot internal retry, missing clocks and stale/no-data fail closed.");
    puts("PASS: separate COMM/INIT/HOLD/DATA, both-source reset, config loss and real STATUS NACK.");
    puts("PASS: zero/unready data is not communication failure; 1-second monitoring never writes or readies.");
    return 0;
}
