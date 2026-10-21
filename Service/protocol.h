/*
 * protocol.h - 串口协议层 (不直接访问硬件, 便于主机侧测试)
 *
 * 上行帧:
 *   $D,<间隙mm>,<温度C>,<状态>\r\n
 *   $R,<ch0>,<ch1>,<比值>,<间隙mm>,<温度C>,<状态>\r\n
 *   OK\r\n / ERR,<code>\r\n / INFO,...\r\n
 *
 * 下行命令:
 *   READ / RAW / INFO / CALS / CLR
 *   RATE,ms / STX,v / ST2,v / TC,k,tref / SWAP,0|1 / CAL,idx,v
 *   RD,reg / WR,reg,val / DUMP      (调试用)
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "measure.h"

#define PROTO_VERSION   "1.0"

void protocol_init(void);

void     protocol_set_rate_ms(uint32_t ms);
uint32_t protocol_get_rate_ms(void);

/* 到上报时间则生成一帧 $R 写入 out, 返回字节数; 未到返回 0 */
int protocol_tick(char *out, int outlen, const measure_result_t *r, uint32_t now_ms);

int protocol_build_d(char *out, int outlen, const measure_result_t *r);
int protocol_build_r(char *out, int outlen, const measure_result_t *r);

/* 处理一条完整命令行, 回复写入 out, 返回回复字节数 (0 = 不应答) */
int protocol_handle_line(const char *line, char *out, int outlen);

#endif /* PROTOCOL_H */
