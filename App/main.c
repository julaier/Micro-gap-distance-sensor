/*
 * main.c - LDC1614 微缝隙测量装置 (STM32F103C8T6)
 *
 * 功能: 通过 LDC1614 双通道差分测量制动器松闸间隙,
 *       结果按 100ms 周期从 USART2 (PA2) 发给上位机 (外部 CH340 模块)。
 *
 * 模块划分见 ../README.md。
 */

#include "app.h"
int main(void)
{
    app_init();

    for (;;) {
        app_poll();
    }
}
 

