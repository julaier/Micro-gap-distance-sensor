#ifndef F103_FLOW_HARD_I2C_H
#define F103_FLOW_HARD_I2C_H
#include <stdint.h>
/* Historical API names ONLY: this adapter uses software I2C.
 * Physical PB7=SCL / PB6=SDA. It never enables an I2C peripheral. */
void hard_i2c_init(void);
uint8_t hard_i2c_lines(void); /* Current bit0=SCL, bit1=SDA; no pull test. */
int hard_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len);
int hard_i2c_write_read(uint8_t addr7, const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen);
#endif
