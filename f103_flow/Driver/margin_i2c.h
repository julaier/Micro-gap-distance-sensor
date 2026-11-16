#ifndef MARGIN_I2C_H
#define MARGIN_I2C_H
#include "wake_i2c.h"

/* TIM14 is 1 MHz. An additional tick compensates for timer quantization.
 * These are scheduling minima, NOT an oscilloscope measurement or exact Hz.
 * TI SNOSCY9A section 6.6: 10..400 kHz, tLOW>=1.3us, tHIGH>=0.6us,
 * tSU;DAT>=0.1us, tHD;DAT>=0, START/STOP>=0.6us, tBUF>=1.3us.
 * tVD;DAT/tVD;ACK are LDC output-valid specifications, not master waits.
 */
#define MARGIN_LOW_US 20u
#define MARGIN_HIGH_US 18u
#define MARGIN_HOLD_US 2u
#define MARGIN_SETUP_US 8u
#define MARGIN_START_US 8u
#define MARGIN_STOP_US 8u
#define MARGIN_FREE_US 10u
#define MARGIN_TIMEOUT_US 2000u

typedef struct {
    uint16_t low_min, low_max, high_min, high_max;
    uint16_t period_min, period_max;
    uint32_t clocks;
    int rc, stop_rc;
    uint8_t captured;
} margin_timing_t;
extern volatile margin_timing_t g_margin_last, g_margin_first_error;
void margin_i2c_report(void); /* Call only outside an I2C transaction. */
#endif
