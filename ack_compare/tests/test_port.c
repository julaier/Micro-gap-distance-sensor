#include <assert.h>
#include <stdio.h>
#include "stm32c0xx.h"
#include "wake_i2c.h"
ack_gpio_stub_t ack_gpio_b;
int main(void)
{
    unsigned value;
    for (value = 0u; value < 65536u; ++value) {
        unsigned expected = ((value & 128u) ? 1u : 0u) | ((value & 64u) ? 2u : 0u);
        ack_gpio_b.IDR = value;
        assert(ack_port_sample() == expected);
#if ACK_COMPARE_PROFILE == 2
        {
            ack_edge_t edge = {0};
            unsigned i;
            ack_gpio_b.BSRR = 0xDEADBEEFu;
            ack_port_capture_edge(&edge);
            assert(ack_gpio_b.BSRR == 128u);
            assert(edge.captured && edge.before == value);
            for (i = 0u; i < 8u; ++i) assert(edge.raw[i] == value);
            assert(ack_gpio_b.IDR == value);
        }
#endif
    }
    puts("PASS: hardware sampler PB7=SCL/PB6=SDA, all 65536 input patterns.");
    return 0;
}
