/* Host check of the real app_init ordering, retries and checkpoint output.
 * Hardware drivers are mocked here; electrical/protocol tests run separately. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../App/app.c"

static char captured[8192];
static size_t captured_len;
static unsigned phase, modules, early_reads, irq_reads, pre_reads;
static unsigned early_probes, delays100, delays1000, fail_rounds, faults;
static int init_result;
static ldc1614_init_trace_t init_trace;

void uart_write(const uint8_t *buf, uint16_t len)
{
    assert(phase != 0u && captured_len + len < sizeof(captured));
    memcpy(captured + captured_len, buf, len);
    captured_len += len;
    captured[captured_len] = '\0';
}
void uart_puts(const char *s) { uart_write((const uint8_t *)s, (uint16_t)strlen(s)); }
void uart_init(uint32_t baud) { (void)baud; assert(!"Must use test_board USART2 setup"); }
void board_init(void) { assert(phase == 0u); phase = 1u; }
int board_clock_is_hse8(void) { return 1; }
void test_delay_ms(uint16_t ms)
{
    assert(phase <= 2u && modules == 0u);
    if (ms == 100u) { assert(phase == 1u); ++delays100; }
    else { assert(ms == 1000u && phase == 2u); ++delays1000; }
}
void ldc1614_init_driver(void)
{
    assert(phase == 1u && delays100 == 1u && modules == 0u);
    phase = 2u;
}
int ldc1614_probe(uint8_t address)
{
    assert(address == 0x2Au); /* No address scan at any checkpoint. */
    if (phase == 2u) ++early_probes;
    else assert(phase == 3u || phase == 4u);
    return 0;
}
int ldc1614_read_reg(uint8_t reg, uint16_t *value)
{
    unsigned *count;
    int fail;
    if (phase == 5u) {
        if (init_result != 0) *value = reg == 0x1Au ? 0x2801u : 0u;
        else switch (reg) {
        case 0x1A: *value = 0x0A01u; break;
        case 0x1B: *value = 0x820Cu; break;
        case 0x08: case 0x09: *value = 0xFFFFu; break;
        case 0x10: case 0x11: *value = 0x0180u; break;
        case 0x19: *value = 0x0001u; break;
        case 0x14: case 0x15: *value = 0x1001u; break;
        default: assert(!"Unexpected config retention register");
        }
        return 0;
    }
    assert(phase >= 2u && phase <= 4u);
    count = phase == 2u ? &early_reads : phase == 3u ? &irq_reads : &pre_reads;
    assert(reg == ((*count & 1u) ? 0x7Fu : 0x7Eu));
    if (phase == 4u) assert(modules == 127u);
    fail = phase == 2u && *count / 2u < fail_rounds;
    ++*count;
    if (fail) return -2;
    *value = reg == 0x7Eu ? 0x5449u : 0x3055u;
    return 0;
}
void hard_i2c_get_diag(hard_i2c_diag_t *d)
{
    memset(d, 0, sizeof(*d));
    d->rc = -2;
    d->stage = HARD_I2C_ADDR_W;
    d->tx_byte = 0x54u;
    d->ack_level = 1u;
}
void ldc1614_get_init_bus_diag(hard_i2c_diag_t *d) { hard_i2c_get_diag(d); }
void ldc1614_set_init_trace(ldc1614_init_trace_t trace)
{
    assert(phase == 4u && modules == 127u && pre_reads == 2u);
    init_trace = trace;
}
void board_start_app_services(void)
{
    unsigned rounds = fail_rounds < 5u ? fail_rounds + 1u : 5u;
    assert(phase == 2u && modules == 0u && early_reads == 2u * rounds);
    assert(early_probes == rounds && delays1000 == rounds - 1u);
    phase = 3u;
}
void button_init(void) { assert(phase == 3u && irq_reads == 2u); phase = 4u; modules = 1u; }
void console_init(void) { assert(modules == 1u); modules |= 2u; }
void adc_temp_init(void) { assert(modules == 3u); modules |= 4u; }
void measure_init(void) { assert(modules == 7u); modules |= 8u; }
void measure_load(void) { assert(modules == 15u); modules |= 16u; }
void protocol_init(void) { assert(modules == 31u); modules |= 32u; }
void protocol_set_rate_ms(uint32_t ms) { assert(ms == 100u && modules == 63u); modules |= 64u; }
int ldc1614_init(uint16_t *did, uint16_t *mid)
{
    ldc1614_init_step_t step = {0};
    assert(did == 0 && mid == 0 && modules == 127u && pre_reads == 2u);
    assert(init_trace != 0);
    step.reg = 0x1Au;
    step.expected = 0x0A01u;
    step.actual = init_result == 0 ? 0x0A01u : 0x2A01u;
    step.result = init_result;
    init_trace(&step);
    phase = 5u;
    return init_result;
}
int ldc1614_init_clock(uint16_t *did, uint16_t *mid, int external)
{
    (void)did; (void)mid; (void)external;
    assert(!"Early reset failure must not trigger a clock fallback");
    return -10;
}
int ldc1614_read_fresh_raw(uint32_t *ch0, uint32_t *ch1,
                          uint16_t *err, uint16_t *status)
{
    assert(phase == 5u && init_result == 0);
    *ch0 = 120000u; *ch1 = 100000u; *err = 0u; *status = 0x004Cu;
    return 0;
}
void board_delay_ms(uint32_t ms) { assert(phase == 5u && ms == 20u); }
void measure_fault(void) { ++faults; }
int32_t adc_temp_read_x10(void) { assert(phase == 5u); return 250; }
void adc_temp_debug(uint16_t *cal, uint16_t *vref, uint16_t *ts) { *cal = *vref = *ts = 0u; }
uint32_t board_millis(void) { assert(phase == 5u); return 10u; }

static void run(unsigned failed_early_rounds, int config_result)
{
    phase = modules = early_reads = irq_reads = pre_reads = early_probes = 0u;
    delays100 = delays1000 = faults = 0u;
    captured_len = 0u;
    captured[0] = '\0';
    fail_rounds = failed_early_rounds;
    init_result = config_result;
    app_init();
    assert(phase == 5u);
    assert(strstr(captured, "$B,FW,C011_SW_I2C_V12_STAGE_DIAG\r\n") != NULL);
    assert(strstr(captured, "$D,IRQ_ID,0,5449,0,3055\r\n") != NULL);
    assert(strstr(captured, "$D,PRE_ID,0,5449,0,3055\r\n") != NULL);
    assert((strstr(captured, "$D,REF_ID,0,5449,0,3055\r\n") != NULL) == (fail_rounds < 5u));
    assert(strstr(captured, "SCAN_BEGIN") == NULL && strstr(captured, "$G,") == NULL);
    assert(faults == (config_result != 0 ? 1u : 0u));
    assert(s_ldc_ready == (config_result == 0 ? 1u : 0u));
    assert(strstr(captured, "$K,BOOT,COMM,0\r\n") != NULL);
    if (config_result == 0) {
        assert(strstr(captured, "$K,EXT40,INIT,0\r\n") != NULL);
        assert(strstr(captured, "$K,EXT40,HOLD,0\r\n") != NULL);
        assert(strstr(captured, "$K,EXT40,DATA,0\r\n") != NULL);
    } else {
        assert(strstr(captured, "$K,EXT40,DATA,-4\r\n") != NULL);
    }
    if (config_result == 0) assert(strstr(captured, "$B,LDC1614 ready\r\n") != NULL);
    else assert(strstr(captured, "$E,10\r\n") != NULL);
}

int main(void)
{
    run(0u, 0);
    run(2u, 0);
    run(5u, -10);
    puts("PASS: test-only startup before app services, bounded ID retries, checkpoints and no default scan.");
    return 0;
}
