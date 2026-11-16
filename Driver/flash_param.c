/*
 * flash_param.c - STM32C011 内部 Flash 参数区
 *
 * STM32C011F6 有 32 KB Flash，页大小 2 KB。参数区占用最后一页
 * 0x08007800 ~ 0x08007FFF。C0 按 64 bit 双字编程。
 */

#include "stm32c0xx.h"
#include "flash_param.h"

#define FLASH_ERROR_FLAGS (FLASH_SR_OPERR | FLASH_SR_PROGERR | \
                           FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                           FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                           FLASH_SR_MISERR | FLASH_SR_FASTERR)

static int flash_wait_ready(void)
{
    uint32_t guard;

    for (guard = 0u; guard < 1000000u; guard++) {
        if ((FLASH->SR & (FLASH_SR_BSY1 | FLASH_SR_CFGBSY)) == 0u) {
            if ((FLASH->SR & FLASH_ERROR_FLAGS) != 0u) {
                FLASH->SR = FLASH_ERROR_FLAGS;
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

const void *flash_param_ptr(void)
{
    return (const void *)(uintptr_t)FLASH_PARAM_ADDR;
}

int flash_param_write(const void *src, uint32_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    uint32_t offset;
    uint32_t page = (FLASH_PARAM_ADDR - FLASH_BASE) / 0x800u;

    if (src == 0 || len == 0u || len > FLASH_PARAM_SIZE) {
        return -1;
    }

    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
    if ((FLASH->CR & FLASH_CR_LOCK) != 0u) {
        return -1;
    }

    FLASH->SR = FLASH_ERROR_FLAGS;
    if (flash_wait_ready() != 0) {
        FLASH->CR |= FLASH_CR_LOCK;
        return -1;
    }

    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) |
                FLASH_CR_PER |
                (page << FLASH_CR_PNB_Pos) |
                FLASH_CR_STRT;
    if (flash_wait_ready() != 0) {
        FLASH->CR &= ~FLASH_CR_PER;
        FLASH->CR |= FLASH_CR_LOCK;
        return -1;
    }
    FLASH->CR &= ~FLASH_CR_PER;

    for (offset = 0u; offset < len; offset += 8u) {
        uint64_t word = 0xFFFFFFFFFFFFFFFFull;
        uint32_t n = len - offset;
        uint32_t i;

        if (n > 8u) {
            n = 8u;
        }
        for (i = 0u; i < n; i++) {
            ((uint8_t *)&word)[i] = in[offset + i];
        }

        FLASH->CR |= FLASH_CR_PG;
        *(volatile uint32_t *)(uintptr_t)(FLASH_PARAM_ADDR + offset) =
            (uint32_t)word;
        __ISB();
        *(volatile uint32_t *)(uintptr_t)(FLASH_PARAM_ADDR + offset + 4u) =
            (uint32_t)(word >> 32u);
        if (flash_wait_ready() != 0) {
            FLASH->CR &= ~FLASH_CR_PG;
            FLASH->CR |= FLASH_CR_LOCK;
            return -1;
        }
        FLASH->CR &= ~FLASH_CR_PG;
    }

    FLASH->CR |= FLASH_CR_LOCK;
    return 0;
}
