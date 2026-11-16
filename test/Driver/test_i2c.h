#ifndef TEST_I2C_H
#define TEST_I2C_H

#include <stdint.h>

enum {
    TEST_I2C_OK = 0,
    TEST_I2C_BUS = -1,
    TEST_I2C_NACK = -2,
    TEST_I2C_ARGUMENT = -3
};

typedef enum {
    TEST_STAGE_IDLE, TEST_STAGE_START, TEST_STAGE_ADDR_W, TEST_STAGE_REG,
    TEST_STAGE_RESTART, TEST_STAGE_ADDR_R, TEST_STAGE_READ_MSB,
    TEST_STAGE_READ_LSB, TEST_STAGE_STOP, TEST_STAGE_DONE
} test_i2c_stage_t;

typedef struct {
    int rc;
    int stop_rc;
    test_i2c_stage_t stage;
    uint8_t tx;
    uint8_t ack_level;  /* Last TX ACK sample: 0=ACK, 1=NACK, 255=not sampled */
    uint8_t lines;      /* Before STOP on failure; idle lines on success */
    uint8_t clocks;
    uint32_t hz;       /* TIM14 estimate for last complete transmitted byte */
} test_i2c_diag_t;

/* Initialization may recover a stuck bus, but never writes a register. */
int test_i2c_init(void);
int test_i2c_probe(uint8_t addr7);
int test_i2c_read_reg(uint8_t addr7, uint8_t reg, uint16_t *value);
void test_i2c_get_diag(test_i2c_diag_t *out);
const char *test_i2c_stage_name(test_i2c_stage_t stage);

#endif
