# Keil 5 实板调试：LDC1614 V5

## 先明确能看到什么

不需要更换或焊接 MCU。用现有 ST-Link 的 SWD 调试 STM32C011。
Keil 可以查看程序阶段、实际软件采样到的 ACK、错误码和缓存的寄存器回读。
它不能代替示波器确定 LDC 供电瞬态、SD 毛刺、CLKIN 波形，或证明线路上没有窄脉冲。
请使用 ST-Link 实板调试，不要选择软件 Simulator 来判断外部 LDC 是否正常。

本版不声称已经找到根因。相对 V4，仅清除 CONFIG bit9，改用 LDC 内部参考，
睡眠 `3401`、唤醒 `1401`。保留 V4 的全电流启动、手动固定驱动及 `IDRIVE=9000`。
I2C 原语、STM32 的 HSE 8 MHz 时钟、其他参数以及原有 5 ms 静默不变。
首次测试请不要修改 `g_wake_quiet`，也不需要改变供电、接线或转接 F103。
本版并不关闭外部 40 MHz 时钟模块，也不启用 bit6 `HIGH_CURRENT_DRV`。NACK 仍算失败。

## 第一次调试：只看完成后的证据

1. 打开 `MDK-ARM/LDC1614_Wake_Test.uvprojx`，确认目标是 `LDC1614_Wake_Test`。
   不要打开父目录正式工程或原 `test` 工程。
2. 编译，确认 0 Error(s)，通过 Flash → Download 手动下载。
   下载会替换 MCU 当前程序；下载器的擦除策略可能影响原末页参数，先保留需要的备份。
3. Options for Target → Debug 选择实际 ST-Link，使用 SWD；保持现有可用连接设置。
   启用调试信息，不必更改整个工程的优化级别，以免同时改变原 I2C 时序。
4. 在 `App/wake_diag.c` 的 `wake_diag_done_checkpoint()` 内可执行语句处按 F9 设断点。
   如源代码断点无法绑定，使用 Debug → Breakpoints，把该函数名作为执行断点表达式。
   清除旧的 I2C 内部断点。断点数量有限，第一遍只需要这一个。
5. Ctrl+F5 进入 Debug，若停在 `main`，按 F5 连续运行；不要逐行执行 I2C。
6. 到完成断点后，View → Watch Windows → Watch 1，添加 `g_wake_debug` 并展开。
   如果没有到达，手动 Stop 后先看 `g_wake_debug.phase` 和 PC 所在函数，不要假定测试完成。

重点观察：

| 字段 | 含义 |
| --- | --- |
| `magic` | 应为 `0x57414B35`，本版 RAM 标记，结构布局与 V3 一致 |
| `done` | 1 表示启动测试结束；0 时快照可能尚未完成 |
| `result_stage` / `result` | 与 `$B,RESULT` 相同；负数不代表有效测量 |
| `quiet_ms` | 默认 5；只有尝试唤醒时才执行这段等待 |
| `sleep_now` / `sleep_1s` | 睡眠时立即与 1 秒后两组证据 |
| `wake_attempted` | 1 才实际尝试了唤醒 |
| `wake_write_rc` | 唤醒写返回值：0 成功，-2 软件采样为 NACK |
| `wake_payload_index` | 0 寄存器地址、1 高字节、2 低字节、255 尚未发 payload |
| `wake_bus.tx` / `ack_level` | 最后发送字节 / ACK 采样电平；0 应答、1 不应答 |
| `wake_bus.stop_rc` | STOP 的独立结果，不覆盖较早的 NACK |
| `wake_read_rc` / `after_config` | 静默后的第一次 CONFIG 回读结果和值 |
| `first_reads` / `plus20ms` | 随后的第一组快照 / 再等 20 ms 后的快照 |
| `mcu_reset_flags` | STM32 的原始 `RCC->CSR2`，不是 LDC 复位原因 |

每组快照先看 `captured`：必须为 1，才表示整组采集结束。
再看 `comm`、`hold`：0 表示本组通信/配置校验通过；`hold=-18` 表示配置不符。
未采集字段的零初始化值不是一次成功读数。读错误时的 `values` 零值也不是测量结果。
`values[]` / `results[]` 索引顺序：

```text
0=1A(CONFIG)  1=1B(MUX)  2=08  3=09  4=10  5=11  6=19
7=14  8=15  9=1E  10=1F  11=18(STATUS)  12=00  13=01  14=02  15=03
```

启动快照不会被后面的 MONITOR 覆盖。不要在 Watch 里修改 `g_wake_debug` 的结果字段。
复位标志可能多个同时置位，也可能受下载/调试复位影响；不能只凭一个标志认定供电故障。
本程序保存标志后清除一次，以免持续累积；不修改选项字节、不启动看门狗。

## 可选：唤醒前的安全断点

在 `wake_diag_ready_checkpoint()` 内设第二个断点。仅在睡眠配置两次校验通过后到达这里，
此时 LDC 仍睡眠，SCL/SDA 没有正在进行的事务，可以看两组睡眠快照。
然后按 F5，一次连续运行到 `wake_diag_done_checkpoint()`。

不要在 `write_byte`、`read_byte`、ACK 采样、延时循环，或唤醒事务与静默/回读之间停住。
MCU 停住时 LDC 不一定停住；这样会改变时间关系。即使睡眠前断点安全，它也会延长
睡眠时间，所以仍需保留一次完全不暂停的串口结果作对照。

`phase` 数值：0空闲、1总线初始化、2软复位、3读ID、4写通道、5读初始CONFIG、
6写睡眠CONFIG、7立即睡眠快照、8一秒睡眠快照、9唤醒前安全点、10唤醒写、
11静默、12首次CONFIG回读、13第一组快照、14二十毫秒后快照、15启动测试结束。
后续只读监控不改变这个启动阶段值。

## 可选：取消静默，做单变量对照

默认 `g_wake_quiet=1`，无需调试器即可运行。若要比较：

1. 从复位重新运行，停在 `main`，必须在 `wake_diag_start()` 调用前。
2. Watch 中将 `g_wake_quiet` 改为 0，其他设置不动，然后 F5 连续运行。
3. 日志应出现 `$B,WAKE_QUIET_MS,0`。这只取消额外 5 ms，不会恢复 V4 的外部参考选择。
4. 每次重新复位后变量默认恢复为 1；运行中修改不会改变已采样的本次设置。

仍然不把“写入 NACK、后来读值正确”算成完整通过。不自动重试、不换参考时钟、
不自动恢复配置，不隐藏故障。即使配置通过，没有线圈也不能据此宣布测量正常。

## 发回哪些证据

保留版本行 `$B,FW,C011_LDC_WAKE_TEST_V5_INTERNAL_REF`，以及
`$B,MODE`（应含 `INTERNAL_REF`）、`$B,ACTIVATE`（应为 `3401/1401`）、
`$B,MCU_RESET_CSR2`、`$B,WAKE_QUIET_MS`、全部 `$W/$S/$C`、
`$K,SLEEP_NOW`、`$K,SLEEP_1S`、`$K,FIRST_READS`、`$K,PLUS20MS`、`$B,RESULT`。
或截图完成断点处展开的 `g_wake_debug`。无需发送几十轮相同的 MONITOR。

参考：Keil 官方 µVision User's Guide 的 Debug Menu and Commands / Watch Window；
TI LDC1614 数据手册 SNOSCY9A 的 CONFIG 寄存器和工作模式章节。
