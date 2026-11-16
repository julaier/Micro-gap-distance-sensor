/*
 * flash_param.h - STM32C011F6 内部 Flash 参数区
 *
 * 占用最后一页 0x08007800，大小 2 KB。
 */
#ifndef FLASH_PARAM_H
#define FLASH_PARAM_H

#include <stdint.h>

#define FLASH_PARAM_ADDR   0x08007800u
#define FLASH_PARAM_SIZE   2048u

const void *flash_param_ptr(void);
int flash_param_write(const void *src, uint32_t len);

#endif /* FLASH_PARAM_H */
