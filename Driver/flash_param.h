/*
 * flash_param.h - 内部 Flash 参数区 (标定表 / 阈值 / 温补系数)
 *
 * 占用最后一页 0x0800FC00, 1KB。STM32F103C8 的最后一页就是 1KB。
 */
#ifndef FLASH_PARAM_H
#define FLASH_PARAM_H

#include <stdint.h>

#define FLASH_PARAM_ADDR   0x0800FC00u
#define FLASH_PARAM_SIZE   1024u

/* 内部 Flash 是内存映射的, 直接读, 不需要解锁 */
const void *flash_param_ptr(void);

/* 先擦页再写入。 返回 0 = 成功 */
int flash_param_write(const void *src, uint32_t len);

#endif /* FLASH_PARAM_H */
