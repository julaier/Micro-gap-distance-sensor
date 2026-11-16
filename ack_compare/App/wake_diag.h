#ifndef WAKE_DIAG_H
#define WAKE_DIAG_H

#include <stdint.h>
#include "wake_i2c.h"

#define WAKE_DIAG_REGISTER_COUNT 16u

enum {
    WAKE_DEBUG_IDLE, WAKE_DEBUG_BUS_INIT, WAKE_DEBUG_RESET, WAKE_DEBUG_ID,
    WAKE_DEBUG_CONFIG_WRITE, WAKE_DEBUG_CONFIG_READ, WAKE_DEBUG_SLEEP_WRITE,
    WAKE_DEBUG_SLEEP_NOW, WAKE_DEBUG_SLEEP_1S, WAKE_DEBUG_READY,
    WAKE_DEBUG_WAKE_WRITE, WAKE_DEBUG_QUIET, WAKE_DEBUG_WAKE_READ,
    WAKE_DEBUG_FIRST_READS, WAKE_DEBUG_PLUS20MS, WAKE_DEBUG_DONE
};

typedef struct {
    uint32_t captured; /* 1 only after the entire snapshot has been collected. */
    uint16_t values[WAKE_DIAG_REGISTER_COUNT];
    int results[WAKE_DIAG_REGISTER_COUNT];
    int hold, comm;
} wake_snapshot_t;

#if ACK_COMPARE_PROFILE == 2
typedef struct {
    uint32_t captured, phase;
    uint8_t operation, reg, payload_index; /* 'R'/'W'; 255 for reads. */
    uint16_t value; /* Requested write value; zero for reads. */
    int rc;
    test_i2c_diag_t bus;
    ack_trace_t ack;
} wake_first_failure_t;
#endif

/* Persistent RAM evidence for Keil Watch. These are software observations,
 * not a voltage trace or an LDC reset-cause register. Startup snapshots are
 * retained unchanged during subsequent read-only monitor rounds. */
typedef struct {
    uint32_t magic, mcu_reset_flags, phase, checkpoint, done, quiet_ms;
    const char *result_stage;
    int result, before_read_rc, sleep_write_rc, wake_write_rc, wake_read_rc;
    uint16_t before_config, after_config;
    uint8_t wake_attempted, wake_payload_index;
    test_i2c_diag_t wake_bus;
    wake_snapshot_t sleep_now, sleep_1s, first_reads, plus20ms;
    ack_trace_t sleep_ack, wake_ack; /* Saved before read transactions overwrite it. */
#if ACK_COMPARE_PROFILE == 2
    wake_first_failure_t first_failure; /* First startup bus error; monitor cannot overwrite. */
#endif
} wake_debug_t;

extern volatile wake_debug_t g_wake_debug;
/* Set ONLY before wake_diag_start (e.g. halted at main): 0=no added wait,
 * nonzero=5ms quiet after the wake transaction. Sampled once per boot. */
extern volatile uint32_t g_wake_quiet;

/* Optional function breakpoints. Never pause inside an I2C transaction.
 * These functions do not halt the CPU unless the debugger sets a breakpoint. */
void wake_diag_ready_checkpoint(void);
void wake_diag_done_checkpoint(void);

/* One reset/configuration attempt per boot. Verify original F103 configuration while
 * asleep immediately and after 1s; only then wake. A second call is refused.
 * Success proves configuration retention only, NOT valid sensor conversion. */
int wake_diag_start(void);
/* Register-pointer writes needed for reads are allowed; no register-data
 * writes, recovery clocks, resets, reinitialization or reference fallback. */
void wake_diag_monitor(uint32_t round);

#endif
