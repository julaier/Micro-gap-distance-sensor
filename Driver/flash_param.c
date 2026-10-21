/*
 * flash_param.c - 内部 Flash 参数区实现
 *
 * 使用 STM32F10x Standard Peripheral Library FLASH API。
 */

#include "stm32f10x.h"
#include "flash_param.h"
// 内部 Flash 是内存映射的, 直接读, 不需要解锁
const void *flash_param_ptr(void)
{
    // 返回参数区的地址, 直接读即可
    return (const void *)(uintptr_t)FLASH_PARAM_ADDR;
}
//src = 要写入的数据, len = 数据长度(字节), 返回 0 = 成功
int flash_param_write(const void *src, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)src;
    uint32_t halfwords;
    uint32_t i;

    if (src == 0 || len == 0u || len > FLASH_PARAM_SIZE) {
        return -1;
    }

    halfwords = (len + 1u) / 2u;   /* 向上取整到半字 */

    FLASH_Unlock();

    if (FLASH_ErasePage(FLASH_PARAM_ADDR) != FLASH_COMPLETE) {
        FLASH_Lock();
        return -1;
    }

    for (i = 0u; i < halfwords; i++) {
        if (FLASH_ProgramHalfWord(FLASH_PARAM_ADDR + (i * 2u), p[i]) != FLASH_COMPLETE) {
            FLASH_Lock();
            return -1;
        }
    }

    FLASH_Lock();
    return 0;
}
