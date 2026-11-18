/*
 * app.h - 应用层 (把各模块串成完整功能)
 *
 * 职责:
 *   1. 上电初始化各模块, 配置 LDC1614;
 *   2. 收到 INTB 后读双通道原始值, 交给 measure 处理;
 *   3. 处理和回复上位机命令;
 *   4. 按设定周期上报 $D 帧。
 */
#ifndef APP_H
#define APP_H

/* 初始化 (不返回错误, 失败时通过串口报文提示) */
void app_init(void);

/* 主循环里反复调用, 不阻塞 */
void app_poll(void);

#endif /* APP_H */
