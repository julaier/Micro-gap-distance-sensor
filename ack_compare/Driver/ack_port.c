#include "stm32c0xx.h"
#include "wake_i2c.h"

uint8_t ack_port_sample(void)
{
    uint32_t pins = GPIOB->IDR; /* Read physical inputs once, never ODR. */
    return (uint8_t)(((pins >> 7) & 1u) | (((pins >> 6) & 1u) << 1));
}

#if ACK_COMPARE_PROFILE == 2
/* Keep timer calls, UART, loops, decoding and decisions OUT of this window.
 * Eight volatile IDR loads follow the atomic SCL release. Stores and bus/CPU
 * latency still exist: this is sparse digital evidence, not a waveform. */
void ack_port_capture_edge(ack_edge_t *out)
{
    out->before = (uint16_t)GPIOB->IDR;
    GPIOB->BSRR = 1u << 7;
    out->raw[0] = (uint16_t)GPIOB->IDR;
    out->raw[1] = (uint16_t)GPIOB->IDR;
    out->raw[2] = (uint16_t)GPIOB->IDR;
    out->raw[3] = (uint16_t)GPIOB->IDR;
    out->raw[4] = (uint16_t)GPIOB->IDR;
    out->raw[5] = (uint16_t)GPIOB->IDR;
    out->raw[6] = (uint16_t)GPIOB->IDR;
    out->raw[7] = (uint16_t)GPIOB->IDR;
    out->captured = 1u;
}
#endif
