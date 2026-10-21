/*
 * button.c - PA0 / PA1 两个按键
 *
 * 按键一端接 GND, 另一端接 PA0/PA1, 内部上拉:
 *   松开 = 高电平, 按下 = 低电平
 * 消抖 20ms, 按住 2 秒算长按。
 */

#include "stm32f10x.h"
#include "button.h"

#define BTN_DEBOUNCE_MS  20u
#define BTN_LONG_MS      2000u

static uint8_t  s_raw[2];       /*最近一次读到的原始电平, 1 = 松开*/
static uint8_t  s_stable[2];    /*消抖后的电平, 0 = 按下*/
static uint32_t s_change_ms[2]; /*原始电平最近一次变化的时间*/
static uint32_t s_press_ms[2];  /*稳定按下的时间*/
static uint8_t  s_long_sent[2]; /*长按事件已经报过*/
static uint8_t  s_short_evt[2];
static uint8_t  s_long_evt[2];

void button_init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /* PA0 / PA1: 输入上拉, 按下接地为低 */
    gpio.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    s_raw[0] = s_stable[0] = 1u;
    s_raw[1] = s_stable[1] = 1u;
}

/* 读按键原始电平: 1 = 松开(高), 0 = 按下(低) */
static uint8_t read_key(int idx)
{
    uint32_t idr = GPIOA->IDR;
    uint32_t pin = (idx == BTN_KEY1) ? (uint32_t)GPIO_Pin_0 : (uint32_t)GPIO_Pin_1;

    return ((idr & pin) != 0u) ? 1u : 0u;
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
                /* 刚按下 */
                s_press_ms[i] = now_ms;
                s_long_sent[i] = 0u;
            } else if (s_long_sent[i] == 0u) {
                /* 松开, 而且没报过长按 -> 算短按 */
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
