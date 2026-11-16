/*
 * button.c - PA0 / PA1 按键
 *
 * 新硬件不再安装按键，但保留原驱动代码。PA0/PA1 配置为内部上拉，
 * 悬空时保持高电平，不会产生短按事件。
 */

#include "stm32c0xx.h"
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_gpio.h"
#include "button.h"

#define BTN_DEBOUNCE_MS  20u
#define BTN_LONG_MS      2000u

static uint8_t  s_raw[2];
static uint8_t  s_stable[2];
static uint32_t s_change_ms[2];
static uint32_t s_press_ms[2];
static uint8_t  s_long_sent[2];
static uint8_t  s_short_evt[2];
static uint8_t  s_long_evt[2];

void button_init(void)
{
    LL_GPIO_InitTypeDef gpio = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    gpio.Pin = LL_GPIO_PIN_0 | LL_GPIO_PIN_1;
    gpio.Mode = LL_GPIO_MODE_INPUT;
    gpio.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOA, &gpio);

    s_raw[0] = s_stable[0] = 1u;
    s_raw[1] = s_stable[1] = 1u;
}

static uint8_t read_key(int idx)
{
    uint32_t pin = (idx == BTN_KEY1) ? LL_GPIO_PIN_0 : LL_GPIO_PIN_1;
    return (LL_GPIO_IsInputPinSet(GPIOA, pin) != 0u) ? 1u : 0u;
}

void button_scan(uint32_t now_ms)
{
    int i;

    for (i = 0; i < 2; i++) {
        uint8_t raw = read_key(i);

        if (raw != s_raw[i]) {
            s_raw[i] = raw;
            s_change_ms[i] = now_ms;
        }
        if (raw != s_stable[i] &&
            (uint32_t)(now_ms - s_change_ms[i]) >= BTN_DEBOUNCE_MS) {
            s_stable[i] = raw;
            if (raw == 0u) {
                s_press_ms[i] = now_ms;
                s_long_sent[i] = 0u;
            } else if (s_long_sent[i] == 0u) {
                s_short_evt[i] = 1u;
            }
        }
        if (s_stable[i] == 0u && s_long_sent[i] == 0u &&
            (uint32_t)(now_ms - s_press_ms[i]) >= BTN_LONG_MS) {
            s_long_evt[i] = 1u;
            s_long_sent[i] = 1u;
        }
    }
}

int button_short_press(int idx)
{
    if (idx < 0 || idx > 1) {
        return 0;
    }
    if (s_short_evt[idx] != 0u) {
        s_short_evt[idx] = 0u;
        return 1;
    }
    return 0;
}

int button_long_press(int idx)
{
    if (idx < 0 || idx > 1) {
        return 0;
    }
    if (s_long_evt[idx] != 0u) {
        s_long_evt[idx] = 0u;
        return 1;
    }
    return 0;
}
