#ifndef SOFT_I2C_PORT_H
#define SOFT_I2C_PORT_H

#include <stdint.h>

/* I2C-only board operations from the working standalone test_board.c.
 * Host tests replace this port, not the production protocol driver.
 */
void i2c_port_init(void);
uint16_t i2c_port_time_us(void);
void i2c_port_delay_us(uint16_t us);
void i2c_port_scl_write(int release);
void i2c_port_sda_write(int release);
void i2c_port_sda_input(int input);
int i2c_port_scl_read(void);
int i2c_port_sda_read(void);
uint8_t i2c_port_lines(void);
uint8_t i2c_port_sda_mode(void);
uint8_t i2c_port_sda_od(void);
uint8_t i2c_port_pull_test(void);

#endif
