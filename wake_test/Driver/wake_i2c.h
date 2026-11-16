#ifndef WAKE_I2C_H
#define WAKE_I2C_H

#include "test_i2c.h"

/* Payload index: 0=register, 1=MSB, 2=LSB; 255=no payload attempted.
 * The return value is never changed by a subsequent diagnostic read. */
int wake_i2c_write_reg(uint8_t addr7, uint8_t reg, uint16_t value,
                       uint8_t *payload_index);

#endif
