# LDC1614 I²C / ACK 对照测试 V6

2026-11-14 补充：独立 C 早期采样工程见 `EDGE_TEST_ZH.md`；硬件工程师排查清单见
`HARDWARE_CHECKLIST_ZH.md`。原 A/B 工程和测试含义保持不变。下面的 HEX 检查脚本现也检查 C，
需先用 `tools/keil_edge_build.cmd` 编译 C；不需要重复烧录 A/B。

这是独立工程，不覆盖正式程序、原始 F103 程序或 `wake_test` V5。
目标：固定原来能工作的 F103 寄存器设置，对照旧软件 I²C 和新的独立软件 I²C。
本机测试/编译不代表实物已经修好；没有线圈时不能验证正常测量。

## 在 Keil5 使用

打开 `MDK-ARM/LDC1614_ACK_Compare.uvprojx`。

1. 首先选择 `LDC1614_ACK_B_OD3`，Rebuild 后手动 Download，退出 Debug 连续运行。
2. 保存从 `$B,FW` 开始到 `$B,READ_ONLY_MONITOR` 的完整串口日志（115200，8N1）。
3. 再选择 `LDC1614_ACK_A_LEGACY`，Rebuild、手动 Download，运行并保存同一段日志。
4. 两次保持接线、供电、外部时钟等条件不变。两种固件都只做一次初始化/唤醒，不自动来回切换。

工程将 B 放在首个 Target；若 Keil 记住了其他 Target，以选择框和串口 PROFILE 为准。
两个目标使用不同输出目录/文件名，避免把上次固件当作本次结果：

- B：`MDK-ARM/Objects_B/LDC1614_ACK_B_OD3.hex`
- A：`MDK-ARM/Objects_A/LDC1614_ACK_A_LEGACY.hex`

启动标识必须是：

```text
$B,FW,C011_LDC_ACK_COMPARE_V6_F103
$B,PROFILE,B_OD3_INDEPENDENT
$B,CONFIG,F103_ORIGINAL,SLEEP,2A01,WAKE,0A01
```

A 的 PROFILE 为 `A_LEGACY_SINGLE`。没有这个版本/PROFILE 标识，不用旧输出判定新固件。
本工程的脚本只编译，不自动烧录，不改选项字节。

## 固定配置：来源于原始 F103

已直接核对 `../../new/Device/ldc1614.c` 的初始化函数，复位 CONFIG 为 `2801` 时：

```c
v &= (uint16_t)~(0x2000u | 0x0200u);
v |= 0x0200u;  /* 结果是 0A01 */
```

两组均使用 `2A01` 睡眠保持、`0A01` 工作；这两值只相差 SLEEP 位。
保留 F103 的外部参考选择、自动校准、自动幅度校正和原激活模式，
不同时更改这些位，也不添加 ERROR_CONFIG 设置。
唤醒后自动控制可能改变 IDRIVE，故不能把 `1E/1F` 不再等于 `9000` 单独判为复位；
睡眠阶段仍比较 IDRIVE 的可写位，活动阶段仍检查这些寄存器读取是否成功。

| 寄存器 | 写入值 | 含义 |
| --- | --- | --- |
| 1C | 8000 | 唯一一次软件复位 |
| 14、15 | 1001 | CH0/CH1 分频 |
| 10、11 | 0180 | 建立时间 |
| 08、09 | FFFF | 转换计数 |
| 1E、1F | 9000 | 初始驱动 |
| 1B | 820C | CH0/CH1 扫描 |
| 1A | 2A01 → 0A01 | 额外睡眠保持诊断 → 原 F103 工作 CONFIG |

除睡眠保持诊断外，以上通道设置/写入顺序来自 F103。
I²C 字节顺序相同：`54,寄存器,高字节,低字节`；读取用 repeated START，MSB 后 ACK、LSB 后 NACK。
CONFIG 是 LDC1614 的配置，不是主控型号配置；同一 LDC/线圈/参考时钟条件下可以复用。
当前没有装线圈，故这里只验证通信、写入及配置保持，不能承诺适合最终线圈参数。

## I²C / ACK 差异

| 项目 | F103 原程序 | A（旧 C0） | B（新 C0） |
| --- | --- | --- | --- |
| 实现 | 硬件 I²C1，配置 100 kHz | 原 test 软件 I²C | 独立重新实现的软件 I²C |
| 引脚 | PB6 SCL、PB7 SDA | PB7 SCL、PB6 SDA | PB7 SCL、PB6 SDA |
| ACK 判断 | 外设的 ACK/AF 标志 | SDA 切输入后，第九拍高电平末段采一次 | SDA 保持开漏输出、释放后，在同一个第九拍高电平采三次 |
| SCL 观测 | 外设处理 | 拉高后等待输入为高 | 等待输入为高，三次 ACK 同时读取 SCL/SDA |
| 三点结果 | 不适用 | 不适用 | 全低 ACK，全高 NACK，混合报 -5；不投票放行 |

A 直接包含未修改的 `wake_test/Driver/wake_i2c.c` 及 `test/Driver/test_i2c.c`；
只在事务结束后复制最后一次原始 ACK，不往旧路径插入逐字节采样。
B 不包含旧底层文件，不在每个 ACK 上切换 MODER，读取的是 IDR，不是 ODR。
`ack_port.c` 用一次 GPIOB IDR 读取同时获取两根线。
B 每次 SCL 拉低后先留 1 µs 数据保持，再修改 SDA；请求数据建立/高电平延时约 5 µs，另有执行开销。
三次 ACK 采样在等待实际 SCL 为高后，分别额外等待 1、2、2 µs；
因此软件时间戳不是精确的 1/3/5 µs，实际耗时由日志报告。
没有把这些延时或 TIM14 估算值称为示波器测出的 100 kHz。

B 同时改变了 SDA 方向处理、数据保持和 ACK 观测方式；若 B 成功、A 失败，
说明应继续在这些差异中定位，不能仅凭该结果说已证明某一处采样错误。
两组均失败也不能直接证明硬件坏了。

## 证据与返回码

一轮流程：复位 → 写通道 → 写 2A01 → 立即快照 → 静等 1 s → 快照 →
通过保持校验才写一次 0A01 → 静等 5 ms → 读回及快照 → 再等 20 ms → 快照。
随后每秒只读监控。没有失败后偷偷复位、恢复总线、重写配置或改参考时钟。
仅首次总线初始化允许在总线被拉低时发恢复时钟；读寄存器必要的寄存器指针写入仍保留。
没有启用 INTB 中断、ADC、OLED、SysTick 或 MCU 看门狗。

`$A` 为缓冲后打印的 ACK 记录，不在传输过程中输出串口：

```text
$A,阶段,字节槽,发送字节,采样次数,SDA掩码,SCL掩码,t0,t1,t2,高电平软件时长,时钟数,结果
```

- 阶段为 SLEEP 或 WAKE；字节槽 0=地址，1=寄存器，2=MSB，3=LSB。
- B 的 WAKE 预期发送 `54 1A 0A 01`，每字节 9 拍，采样次数 3。
- 掩码按 bit0/bit1/bit2 表示早/中/晚的电平。SDA=0、SCL=7 表示三点均 ACK 且 SCL 高；SDA=7 表示三点均 NACK。
- SDA 为 1～6 时返回 -5（不稳定）；SCL 不是 7 时返回 -1。
- A 只报告最后发送字节的原始单点 SDA。SCL=255、时间=65535 表示**没有测量**，不是线真的异常。
- 时间单位 µs，来源于 TIM14 软件观测。采样开销本身会改变时序，无法观察两采样点之间的短毛刺或电压幅度。
- `$W` 是整个写事务结果，含 STOP 状态。即使每字节 ACK，STOP 失败仍会报错。
- -2=采到 NACK，-18=本测试读回/保持不符，均为程序返回码，不是 LDC 自带的故障码。
- `CONFIG_PASS_NOT_MEASUREMENT,0` 只表示本轮配置通信/保持通过，不表示正常测量。

请优先提供启动整段，尤其 `$W,1A`、`$A,SLEEP`、`$A,WAKE`、`$C,1A`、`$B,RESULT`。
只截取后面的 MONITOR 无法还原失败字节的 ACK 过程。

## Keil Debug

Watch 添加 `g_wake_debug`，可看 `sleep_ack`、`wake_ack`、`wake_bus`、`wake_payload_index`、
四组快照及 `result`。这些启动证据不会被后续 monitor 覆盖。
可在 `wake_diag_ready_checkpoint`（仍在睡眠，准备唤醒）或
`wake_diag_done_checkpoint`（证据已保存）打函数断点。
从前一个断点按 F5 连续运行到后一个，不要在 I²C 位传输/ACK 内单步或暂停；
暂停会改变时序，不能把单步结果当正常运行结果。

## 本机验证

```powershell
.\tools\run_tests.ps1
cmd /c tools\keil_build.cmd rebuild
.\tools\verify_hex.ps1
```

主机测试执行 A、B 实际驱动及诊断代码，对 GPIO 边沿解码；覆盖地址/字节序、
每个 NACK 位置、超时/STOP、计时回绕、睡眠保持门限、自动 IDRIVE 变化、
复位样读回、静默间隔、只读监控、三点 ACK 的所有混合模式和 SCL 采样异常。
另对实际 `ack_port.c` 测试全部 65536 个低 16 位输入组合。
这些数字模型不验证实物上升沿、电压、短毛刺或真正的传感器启动。

资料入口：TI LDC1614 数据手册 CONFIG 表、ST RM0490 的 GPIO 输出/输入数据寄存器章节。
本次不因尚未实测的新实现而断言硬件损坏。
