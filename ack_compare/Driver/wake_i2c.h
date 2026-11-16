#ifndef ACK_COMPARE_I2C_H
#define ACK_COMPARE_I2C_H

#include "test_i2c.h"

#ifndef ACK_COMPARE_PROFILE
#define ACK_COMPARE_PROFILE 1
#endif
#if ACK_COMPARE_PROFILE == 0
#define ACK_PROFILE_NAME "A_LEGACY_SINGLE"
#elif ACK_COMPARE_PROFILE == 1
#define ACK_PROFILE_NAME "B_OD3_INDEPENDENT"
#elif ACK_COMPARE_PROFILE == 2
#define ACK_PROFILE_NAME "C_EDGE_EARLY"
#else
#error Invalid ACK_COMPARE_PROFILE
#endif

/* Mixed SDA during the ninth high is NOT accepted by majority vote. */
#define ACK_I2C_UNSTABLE (-5)
#define ACK_TIME_UNKNOWN 65535u
#if ACK_COMPARE_PROFILE == 2
/* Raw low 16 bits of physical GPIOB IDR, not ODR or decoded ACK votes.
 * PB7=SCL, PB6=SDA. No fixed time spacing is asserted for these reads. */
typedef struct {
    uint16_t before, raw[8];
    uint8_t captured;
} ack_edge_t;
void ack_port_capture_edge(ack_edge_t *out);
#endif
typedef struct {
    uint8_t slot, tx, samples, sda_mask, scl_mask, clocks;
    uint16_t dt_us[3], high_us;
    int rc;
#if ACK_COMPARE_PROFILE == 2
    ack_edge_t edge;
#endif
} ack_sample_t;
typedef struct {
    unsigned count;
    ack_sample_t bytes[4];
} ack_trace_t;

/* 0=register, 1=MSB, 2=LSB; 255=payload not reached. */
int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value,
                       uint8_t *payload_index);
void ack_i2c_get_trace(volatile ack_trace_t *out);
/* Single GPIOB IDR snapshot: bit0=PB7 SCL, bit1=PB6 SDA. */
uint8_t ack_port_sample(void);

#endif
