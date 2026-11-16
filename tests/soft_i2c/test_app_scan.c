/* Test the real app scanner without linking unrelated board functions:
 * gcc -std=c99 -O2 -fwhole-program -Wall -Wextra -Werror
 *     -ffunction-sections -fdata-sections
 *     -IApp -IDriver -IDevice -IService tests/soft_i2c/test_app_scan.c
 *     '-Wl,--gc-sections' -o <test.exe>
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_I2C_SCAN 1 /* Historical scanner is disabled in production. */
#include "../../App/app.c"

static char captured[16384];
static size_t captured_len;
static unsigned probes;
static int responding_address, failing_address;
static hard_i2c_diag_t probe_diag;

void uart_write(const uint8_t *buf, uint16_t len)
{
    assert(captured_len + len < sizeof(captured));
    memcpy(captured + captured_len, buf, len);
    captured_len += len;
    captured[captured_len] = '\0';
}

void uart_puts(const char *s)
{
    uart_write((const uint8_t *)s, (uint16_t)strlen(s));
}

int ldc1614_probe(uint8_t address)
{
    int rc;
    assert(address == 0x08u + probes && address <= 0x77u);
    probes++;
    rc = failing_address == -2 || address == failing_address ? -1 :
         address == responding_address ? 0 : -2;
    memset(&probe_diag, 0, sizeof(probe_diag));
    probe_diag.rc = rc;
    probe_diag.stage = rc == -1 ? HARD_I2C_START :
                       rc == 0 ? HARD_I2C_DONE : HARD_I2C_ADDR_W;
    probe_diag.tx_byte = rc == -1 ? 0u : (uint8_t)(address << 1);
    probe_diag.ack_level = rc == -1 ? 255u : rc == 0 ? 0u : 1u;
    probe_diag.lines = rc == 0 ? 3u : 2u;
    return rc;
}

void hard_i2c_get_diag(hard_i2c_diag_t *diag)
{
    *diag = probe_diag;
}

static void run_scan(int response, int failure)
{
    const char *line;
    unsigned rows = 0u, acks = 0u, nacks = 0u, errors = 0u;
    char summary[80];

    captured_len = probes = 0u;
    captured[0] = '\0';
    responding_address = response;
    failing_address = failure;
    report_i2c_scan();
    assert(probes == 112u);
    assert(strncmp(captured, "$B,SCAN_BEGIN,08,77,W\r\n", 23u) == 0);
    line = captured;
    while ((line = strstr(line, "$S,")) != NULL) {
        unsigned address, tx, last_tx, ack_level, lines;
        int rc, expected_rc;
        char stage[16];
        assert(sscanf(line, "$S,%x,%x,%d,%15[^,],%x,%u,%u",
                      &address, &tx, &rc, stage, &last_tx, &ack_level, &lines) == 7);
        assert(address == 0x08u + rows && address <= 0x77u);
        assert(tx == address << 1);
        expected_rc = failure == -2 || (int)address == failure ? -1 :
                      (int)address == response ? 0 : -2;
        assert(rc == expected_rc);
        assert(strcmp(stage, rc == -1 ? "START" : rc == 0 ? "DONE" : "ADDR_W") == 0);
        assert(last_tx == (rc == -1 ? 0u : tx));
        assert(ack_level == (rc == -1 ? 255u : rc == 0 ? 0u : 1u));
        assert(lines == (rc == 0 ? 3u : 2u));
        if (rc == 0) acks++;
        else if (rc == -2) nacks++;
        else errors++;
        rows++;
        line = strchr(line, '\n');
        assert(line != NULL);
        line++;
    }
    assert(rows == 112u && acks + nacks + errors == rows);
    snprintf(summary, sizeof(summary), "$B,SCAN_DONE,112,%u,%u,%u\r\n",
             acks, nacks, errors);
    assert(strstr(captured, summary) != NULL);
    assert(strstr(captured, "$P,2A,") != NULL && strstr(captured, "$P,2D,") != NULL);
    assert(LDC1614_I2C_ADDR == 0x2Au); /* A scan does not retarget the device. */
}

int main(void)
{
    run_scan(0x2A, -1);
    run_scan(0x54, -1);
    run_scan(0x08, -1);
    run_scan(0x77, 0x08);
    run_scan(-1, -1);
    run_scan(-1, -2);
    puts("PASS: app scans all 112 ordinary addresses once, skips reserved addresses,");
    puts("      reports shifted bytes, ACK/NACK/errors and correct summary counts.");
    return 0;
}
