#include <assert.h>
#include <stdio.h>
#include "margin_port.h"
margin_gpio_stub_t margin_gpio_b;
margin_timer_stub_t margin_timer14;
uint32_t margin_stub_mask;
int main(void)
{
    unsigned n;
    for (n = 0u; n < 65536u; ++n) {
        unsigned expected = ((n >> 7) & 1u) | (((n >> 6) & 1u) << 1);
        margin_gpio_b.IDR = n; margin_timer14.CNT = n + 65536u;
        assert(margin_pins() == expected && margin_now() == n);
        margin_scl(0); assert(margin_gpio_b.BSRR == (1u << 23));
        margin_scl(1); assert(margin_gpio_b.BSRR == (1u << 7));
        margin_sda(0); assert(margin_gpio_b.BSRR == (1u << 22));
        margin_sda(1); assert(margin_gpio_b.BSRR == (1u << 6));
        assert(margin_gpio_b.IDR == n);
    }
    for (n = 0u; n < 2u; ++n) {
        uint32_t prior;
        margin_stub_mask = n; prior = margin_lock();
        assert(prior == n && margin_stub_mask == 1u);
        margin_unlock(prior); assert(margin_stub_mask == n);
    }
    puts("PASS margin actual port: all 65536 IDR patterns, PB7/PB6 BSRR mapping, TIM14 truncation, PRIMASK restore.");
    return 0;
}
