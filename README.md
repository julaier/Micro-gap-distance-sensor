# LDC1614 微缝隙测量固件（STM32C011F6U6TR）

2026-11-14 新增独立原 F103 初始化流程验证版：见 `f103_flow/README.md`。
保持物理 PB7=SCL、PB6=SDA 和 B 版软件 I²C，直接复用原 F103 LDC 初始化源码，
不加 `2A01` 睡眠诊断步骤；单目标工程为 `f103_flow/MDK-ARM/LDC1614_F103_Flow.uvprojx`。
这是配置/原始数据验证版，不替换下述正式应用，也不覆盖已有 A/B/C 测试。

本工程由原 STM32F103C8T6 固件迁移到 STM32C011F6U6TR，使用
STM32C0 LL 低层库。LDC1614 优先使用外部 40 MHz，有启动故障时尝试内部参考，MCU 使用
外部 8 MHz 晶振直接运行，不使用 PLL（STM32C011 没有 PLL）。

Keil 工程：`MDK-ARM\LDC1614_C011.uvprojx`

需要 Keil MDK、ARM Compiler 6 和
`Keil.STM32C0xx_DFP 2.4.0` 设备支持包。

## 一、硬件连接

| 功能 | STM32C011F6U6TR 引脚 | 说明 |
| --- | --- | --- |
| 主控 | STM32C011F6U6TR | Cortex-M0+，32 KB Flash，6 KB SRAM |
| MCU 时钟 | PC14 / PC15 | 外部 8 MHz 晶振，HSE 直接作为 SYSCLK |
| USART2_TX | PA2 / AF1 | 接 CH340 RXD |
| USART2_RX | PA3 / AF1 | 接 CH340 TXD |
| LDC1614 INTB | PA4 | 下降沿触发，EXTI4_15 中断 |
| 软件 I2C SCL | PB7（19 脚） | GPIO 开漏，与 LDC1614 1 脚连接，4.7 kΩ 上拉 |
| 软件 I2C SDA | PB6（18 脚） | GPIO 开漏，与 LDC1614 2 脚连接，4.7 kΩ 上拉 |
| LDC1614 CLKIN | 外部 40 MHz | 有源晶振单端输入 |
| LDC1614 ADDR | GND | 7 位地址 0x2A |
| LDC1614 SD | GND | 正常工作 |
| SWD | PA13 / PA14 | SWDIO / SWCLK |

新硬件不再安装 OLED 和 PA0/PA1 按键。相关源码仍保留，OLED 初始化和
显示调用已注释；按键驱动仍保留但悬空输入不会产生按键事件。

按实物接线使用 PB7=SCL、PB6=SDA，地址固定为已验证的 `0x2A`。
当前版本 **V12_STAGE_DIAG** 保留 V9 已在用户板上读通 ID 的原测试底层，
修正后续配置和运行故障处理：

- 正式 Keil 工程直接编译 `test/Driver/test_board.c`，`hard_i2c.c` 原样包含 `test_i2c.c`；成功的 GPIO/收发/延时基元不改动。
- TIM14 1 MHz，半拍最小等待 5 μs；PB7=SCL、PB6=SDA；不调频、不换引脚、不扫描地址。
- 保留 `REF_ID`、`IRQ_ID`、`PRE_ID` 三个检查点。用户 V9 日志已证实三个 ID 检查全部成功，失败转移到 CONFIG 最后一个数据字节，不再属于地址无应答。
- 外部 CONFIG 拆成两笔：保持休眠选择外部参考（`2A01`）→ 等待/回读 → 清休眠位（`0A01`）→ 等待/回读。
- 初始化中的分频、建立时间、转换计数、驱动电流、MUX、ERROR_CONFIG、CONFIG 均回读校验；保留旧工程最终采样参数和幅度/电流配置。
- 仅针对 CONFIG 最后一个数据字节的 NACK，在 STOP 成功且完整回读精确匹配时允许继续，并明确记录 `NACK_VERIFIED`。地址/前缀 NACK、超时、STOP 失败、回读失败或不匹配仍报故障。
- 设置 `ERROR_CONFIG.DRDY_2INT=1`，使 PA4 不只是开启 MCU 中断，也实际接收 LDC 数据就绪信号；保留 20 ms 主动轮询。
- CONFIG/启动转换失败时，先回读 CONFIG、MUX、RCOUNT0、SETTLECOUNT0、ERROR_CONFIG、CLOCK_DIVIDERS0，保留现场，然后完整软复位、重写所有配置，用内部参考重试一次（`2801` → `0801`）。ID、软复位或其他前置配置错误不触发时钟重试。
- 两种参考均须通过两组新鲜、非零双通道数据检查才输出 ready；独立于 INTB，每 20 ms 检查一次，共最多 26 次、25 次等待（等待合计 500 ms，另加通信耗时）。仅寄存器读通而没有转换不能通过。
- 运行中每次 INTB/20 ms 轮询先检查 `STATUS.UNREADCONV0/1`；未同时就绪不重复处理旧数据，只有新鲜且非零、无已报告 DATA 错误的双通道数据才刷新时间戳，持续 500 ms 无新数据判故障。
- V12 将 `COMM`（ID 通信）、`INIT`（完整配置及唤醒）、`HOLD`（唤醒后稳定配置保持）、`DATA`（两组有效新转换）分别输出为 `$K`。没有有效数据不再冒充 I2C 错误；真正的转换读取总线错误保留负返回码，不统一改报无数据。
- `HOLD` 检查 CONFIG、MUX、两路 RCOUNT、SETTLECOUNT、CLOCK_DIVIDERS 和 ERROR_CONFIG，共 9 个稳定寄存器。自动驱动校准可能改变 DRIVE_CURRENT 字段，因此不把其唤醒后的变化误判为配置丢失。
- 最终启动未就绪时输出 `LDC_DIAG_ONLY`，每秒只读 ID、上述配置、STATUS 和两路完整 DATA 寄存器；不自动复位、不重新配置、不因读到零值或旧值而进入测量。装好传感器后重新上电，重新进行完整测量验证。
- 原测量算法、9 点代码标定表、串口协议与 `../new/` 对应文件逐字节一致；按键代码保留，OLED 调用继续注释。

用户 V10 日志显示所有休眠配置回读正确，外部休眠 CONFIG=`2A01` 也通过；
仅写入唤醒 `0A01` 时末字节 NACK，随后回读 `2801`（CONFIG 复位默认值）。
这是启动转换失败，不是 INTB 没触发或地址未应答。**只凭 CONFIG 不能确认整片复位，
更不能认定外部振荡器损坏**；新增 `$Q` 回读可区分其他配置是否也丢失。
V11 的时钟对照只改变 REF_CLK_SRC，保持幅度、电流、分频等设置不变，
两次尝试均完整复位和配置；若内部可用而外部不可用，外部参考路径的嫌疑提高，
但仍不是对硬件根因的最终证明。若两者均回到默认配置，需重点检查供电、SD 及复位路径。

内部参考不是精确 40 MHz（数据手册典型约 43 MHz），温漂、采样间隔与原始值尺度均可能变化。
保留原比值算法/标定表用于对比，**不保证内部参考下原距离精度，应实测核验，必要时重新标定**。
本版本已通过本机联测与 Keil 编译，**尚未在用户板上确认完整采样恢复**。
用户当前 CH0/CH1 线圈尚未安装：可以继续验证通信和配置保持，但不能验证正常距离测量。
`DATA=1` 不等于确认缺线圈，也可能是参考时钟、传感器或其他转换故障。

独立 `test/` 测试源码保持原样，正式工程依赖 `test/Driver/`，不要删除或移动。
不要将 `test_i2c.c` 再独立加入正式目标，否则重复定义。

## 二、目录结构

```text
STM32C011F6U6TR/
  App/
    main.c / app.c              启动、轮询、串口上报
    display.c / console.c       原 OLED 显示层、串口命令层
  Device/
    ldc1614.c / ldc1614.h       LDC1614 寄存器与双通道读取
  Driver/
    board.c                     调用原 test 启动，延后启动 SysTick/PA4 EXTI
    hard_i2c.c                  原样包含 test_i2c.c，适配原应用 API
    soft_i2c_port.c             V8 历史文件，V9 不编译
    uart.c                      USART2 LL 查询收发
    adc_temp.c                  ADC1 内部温度与 VREFINT
    button.c                    PA0/PA1 按键（保留）
    flash_param.c               最后一页 Flash 参数区
    oled.c                      OLED 驱动（保留但不初始化）
  Service/
    measure.c                   滤波、标定、温补和状态判断
    protocol.c                  串口帧组包
  Drivers/
    STM32C0xx_HAL_Driver/       STM32C0 LL 源码与头文件
    CMSIS/                      Cortex-M0+ 与 STM32C011 设备文件
  test/Driver/
    test_board.c / test_i2c.c   正式固件直接复用，原文件保持不变
  Startup/
    startup_stm32c011xx.s       C011 启动文件
  MDK-ARM/
    LDC1614_C011.uvprojx
  tools/keil_build.cmd
```

## 三、时钟与中断

`Driver/board.c` 调用原 `test_board_init()`：先显式设置 HSI 分频为 1，
切到 HSI 48 MHz，再尝试启动 HSE。
HSE 正常时切换到外部 8 MHz 并作为最终系统时钟；HSE 失败时保留
HSI 48 MHz，启动报文会显示 `$B,BOOT,CLOCK_UNEXPECTED`。

正常启动关键信息示例（省略部分诊断行，并非本次实板记录）：

```text
$B,BOOT,HSE8M
$B,FW,C011_SW_I2C_V12_STAGE_DIAG
$B,I2C_SOURCE,ORIGINAL_TEST,ADDR,2A
$D,REF_ID,0,5449,0,3055
$D,IRQ_ID,0,5449,0,3055
$D,PRE_ID,0,5449,0,3055
$K,BOOT,COMM,0
$B,LDC_START,EXT40
$K,EXT40,INIT,0
$V,EXT40,2,0,004C,120000,100000,0000
$K,EXT40,HOLD,0
$K,EXT40,DATA,0
$B,LDC_CLOCK,EXT40
$B,LDC1614 ready
```

`REF_ID` 完成后才启用 SysTick 和 `PA4` 的 `EXTI4_15_IRQHandler` 下降沿中断。
首次检查只使用 TIM14 轮询延时，不依赖尚未启动的 SysTick。

## 四、编译

在工程根目录执行：

```bat
tools\keil_build.cmd build
tools\keil_build.cmd rebuild
tools\keil_build.cmd flash
```

当前验证结果：

```text
Code=29936  RO-data=604  RW-data=8  ZI-data=2360
0 Error(s), 0 Warning(s)
```

输出文件：

```text
MDK-ARM\Objects\LDC1614_C011.axf
MDK-ARM\Objects\LDC1614_C011.hex
```

## 五、串口协议

USART2 使用 `115200 8N1`，每 100 ms 上报一帧：

```text
$R,<ch0>,<ch1>,<比值>,<距离mm>,<温度C>,<状态>\r\n
```

状态码：

| 值 | 含义 |
| ---: | --- |
| 0 | 正常 |
| 1 | 提醒 |
| 2 | 报警 |
| 3 | 故障 |
| 4 | 未标定 |

V12 启动版本标识：

```text
$B,FW,C011_SW_I2C_V12_STAGE_DIAG
$B,I2C_SOURCE,ORIGINAL_TEST,ADDR,2A
```

启动检查顺序：

1. `$D,REF_ID,<mid_rc>,<mid_hex>,<did_rc>,<did_hex>`：原测试的时钟、GPIO、USART2、TIM14，尚未启动 SysTick、EXTI、ADC、按键等。失败最多尝试 5 轮，间隔 1 秒。
2. `$D,IRQ_ID,...`：启动 SysTick/INTB 后再次读取。
3. `$D,PRE_ID,...`：其余应用初始化完成后、LDC 软复位及配置写入之前读取。
4. 执行外部参考初始化、配置回读及两组新数据验证；CONFIG、配置保持或无有效转换时保留现场，再完整复位配置，用内部参考重试一次；最终成功才输出 `$B,LDC1614 ready`，最终失败输出 `$E,<错误码>` 和 `$B,LDC_DIAG_ONLY`，随后每秒只读监测。

每个检查点成功应为 `0,5449,0,3055`。失败的 `0000` 是占位值，
不是成功读取的寄存器内容。若前一个检查点成功、后一个失败，可据此缩小差异范围，
但不能仅凭阶段名就断言某中断或外设是根因。请保留从复位开始的完整日志。

每笔初始化配置输出：

```text
$C,<reg_hex>,<expected_hex>,<write_rc>,<read_rc>,<actual_hex>,<OK|NACK_VERIFIED|FAIL>
```

正常 CONFIG 示例：

```text
$C,1A,2A01,0,0,2A01,OK
$C,1A,0A01,0,0,0A01,OK
$V,EXT40,2,0,004C,120000,100000,0000
$B,LDC1614 ready
```

`read_rc=-4` 表示尚未尝试回读，失败的 `actual=0000` 可能只是占位值。
`NACK_VERIFIED` 不表示总线没有 NACK，而是如实保留 `write_rc=-2`，
且随后完整回读与目标值相同。只收到 ACK 但回读错误也不能通过。
初始化失败时 `$X/$T/$W,INIT` 保留首次失败现场，不被后续回读覆盖。
`E,17` 表示最终 CONFIG 阶段仍失败，`E,18` 表示配置保持/精确校验失败，
`E,19` 表示数据就绪中断配置失败，
`E,20` 表示最终未能在限定次数内取得两组新鲜且非零、无已报告 DATA 错误的双通道数据。
转换验证期间真实的总线错误直接保留，例如 STATUS 读地址 NACK 报 `DATA=-2` / `E,2`，不是 `DATA=1` / `E,20`。

V12 分阶段诊断：

```text
$K,<BOOT|EXT40|INTERNAL>,<COMM|INIT|HOLD|DATA>,<rc>
```

| 阶段 | 含义 |
| --- | --- |
| COMM | ID 是否正确；0=通过，-11=失败，具体读返回码见同次 `$D` |
| INIT | 完整配置和唤醒的结果；0=通过，负数=对应初始化错误 |
| HOLD | 9 个稳定配置是否仍与本次目标一致；0=通过，-18=读成功但值不符，其他负数=配置读通信失败 |
| DATA | 0=两组有效新数据，1=未取得两组有效新数据，-4=初始化失败因而未尝试，其他负数=数据读取通信失败 |

`INIT=0` 和 `HOLD=0` 不依赖线圈产生非零测量值，也不证明传感器和参考时钟正常。
只有初始化、配置保持、有效新数据三项全部通过才输出 ready。
例如无线圈可能出现以下合成示例，实际日志也可能表现为未就绪而非零数据：

```text
$K,BOOT,COMM,0
$K,INTERNAL,INIT,0
$K,INTERNAL,HOLD,0
$K,INTERNAL,DATA,1
$E,20
$B,LDC_DIAG_ONLY
```

这表示通信和配置保持通过，但测量未通过；不是通信失败，也不能仅凭该结果确定根因。
如果仍出现 `INIT=-17`、`HOLD=-18`、`DATA=-4`，唤醒/配置丢失问题仍存在，不会被无线圈诊断放过。

新增启动诊断：

```text
$B,LDC_START,<EXT40|INTERNAL>
$B,LDC_ATTEMPT,<EXT40|INTERNAL>,<failed_rc>
$Q,<EXT40|INTERNAL>,<reg_hex>,<read_rc>,<value_hex>
$B,LDC_FALLBACK,INTERNAL,RECHECK_CAL
$V,<EXT40|INTERNAL>,<good_pairs>,<last_sample_rc>,<status_hex>,<ch0>,<ch1>,<data_err_hex>
$B,LDC_CLOCK,<EXT40|INTERNAL>[,RECHECK_CAL]
```

`$Q` 在重试复位前输出，最终未就绪后也会每秒输出；不能把 `read_rc!=0` 时的占位 `0000` 当作默认值。
例如 `CONFIG=2801` 且 `MUX=020F`、`RCOUNT0=0080`、`SETTLECOUNT0=0000`、
`ERROR_CONFIG=0000` 都成功读回，才有多寄存器恢复复位值的证据；
如果后几项仍是 `820C/FFFF/0180/0001`，只能说明 CONFIG 有变化。
`$V` 中 `last_sample_rc=1` 表示两路未同时就绪，不是 I2C NACK；
`good_pairs=2` 才通过。固定目标下数值可以完全相同，依靠 UNREADCONV 判定新数据。
`RECHECK_CAL` 提示内部参考与原外部参考性能不等价，需核验距离精度。

V12 的 `$Q` 顺序是 `1A/1B/08/09/10/11/19/14/15/18/00/01/02/03`。
`18` 为 STATUS；`00+01`、`02+03` 为 CH0、CH1 的 MSB+LSB，全部按 4 位十六进制显示。
后五项是未就绪时的原始只读快照，不要求 UNREADCONV；读取 DATA 会消耗相应未读标志，
这些读数不得用于宣称新鲜数据、传感器存在或测量正常。
`DIAG_ID` 为每秒重新读取的 ID，随后 `$K,...,COMM` 只表示这一次 ID 检查结果。
诊断本身不发送寄存器数据写入、不调用 I2C 恢复或器件复位；原 `WR/CALS/CLR` 等明确串口命令仍保留原有功能。

本次仍保持 `ERROR_CONFIG=0001`，没有顺带更改幅度/电流或错误报告配置。
因此未报告 DATA 错误并不证明幅度正常。STM32 看门狗仍关闭；LDC 传感器看门狗是另一机制，
TI 说明其超时只发生在连续模式，当前 CH0/CH1 顺序扫描下应检查零计数和幅度问题。

总线失败诊断：

```text
$X,<tag>,<rc>,<stage>,<byte_index>,<lines>
$T,<tag>,<clocks>,<estimated_hz>,<half_delay_us>,<timer_hz>
$W,<tag>,<last_tx_hex>,<ack_level>,<sda_mode>,<sda_od>,<stop_rc>
```

- `rc=0` 成功，`-2` NACK，`-1` 总线/超时，`-3` 参数错误。
- `ack_level=0` ACK、`1` NACK、`255` 未采样；不忽略 NACK。
- `sda_mode/sda_od` 恒为 `255`，表示原测试代码不额外采集这两个诊断字段，不表示实际 GPIO 模式为 255。原代码在 ACK 和读数据时显式设置 SDA 输入。
- `stop_rc` 单独报告 STOP 检查，不覆盖前面的首次错误。
- `lines` 的 bit0=SCL(PB7)、bit1=SDA(PB6)。失败时是 STOP 前现场，`2` 不能单独解释为 SCL 永久卡低。
- `clocks/estimated_hz` 仅属于最近发送字节：8 位加 ACK 共 9 拍，以其中 8 周期估算。不是示波器实测，也不统计接收字节。
- `half_delay_us=5`，`timer_hz=1000000`。旧周期最小值/最大值和总周期字段未采集，保留为 0。
- 不再运行主动 GPIO/上下拉自检及全地址扫描，不输出 `$G`、`$I` 或 `SCAN_BEGIN`。历史扫描函数保留在默认关闭的 `APP_I2C_SCAN` 后。
- 旧 `hard_i2c_lines()` 上下拉测试 API 返回 `255` 表示未采集；正常初始化不调用它。

### 本机回归测试

```powershell
.\tools\run_i2c_tests.ps1
```

当前测试包括：

1. 原 `test_i2c.c` 与正式适配接口在边沿解码从机模型上的收发、ACK、重复 START、末字节 NACK 和 STOP；比较的是适配器与同一原源码直接调用，不是两个独立实现。
2. 真实 LDC 驱动：分阶段初始化、所有配置回读、双通道数据；12 个休眠/唤醒 CONFIG 注入故障，涵盖末字节拒收、写入但 NACK、回读不符、回读 NACK、前缀 NACK、STOP 失败。
3. `test_app_startup.c`：真实 `app_init()` 的初始化顺序、早期重试上限、三个检查点、成功/失败日志；确认不提前调用正式 UART 初始化或其他应用模块，不默认扫描。
4. 历史扫描函数的边界、输出及汇总计数；这只是测试覆盖，不表示正式启动会扫描。
5. `test_app_integration.c` 联合真实应用、I2C、LDC、测量、协议和命令接收层，模型注入双通道数据，验证原 9 点标定、正常/提醒/报警、INTB/轮询、串口帧、命令、故障与恢复、初始化失败不进入测量。Flash 保存只使用内存模型，不代表已验证实板擦写。
6. 原 `test/tools/run_tests.ps1` 继续通过，独立测试源码不变。
7. V11 使用时钟感知数字转换模型，覆盖唤醒 NACK+整片复位镜像、CONFIG 单独丢失、外部不产生转换但内部可用、两种参考均无转换、仅一组旧数据、停止产生新数据后超时、双时钟 CONFIG 均失败。模型故障是合成测试，不代表已确认的实板原因。
8. V12 进一步覆盖两种参考唤醒均出现完整复位镜像、CONFIG 正确但 RCOUNT 丢失、9 个稳定配置逐项损坏、自动驱动字段变化、真实 STATUS 读地址 NACK、零数据/未就绪数据、1 秒只读诊断与监测中目标失联。确认监测不增加寄存器写事务、不更新成功采样时间戳、不输出假 ready，正常旧测量/协议回归仍通过。

V8 的 `test_port_registers.c` 和 `soft_i2c_port.c` 仅作为历史文件保留，
不再是正式固件或当前回归入口。V6/V7 的调频测试同样不作为当前验证。

本机模型不模拟模拟上升沿、焊接和供电，不能代替实板验证。

## 六、采样与处理

1. LDC1614 自动扫描 CH0 和 CH1；
2. 一轮转换完成时 INTB 拉低，PA4 触发 EXTI；
3. INTB 或 20 ms 轮询时确认两路 UNREADCONV 均置位，再读取 CH0/CH1 的 28 位原始数据；
4. CH0/CH1 比值经过中值和滑动平均；
5. 经温度修正、标定插值、迟滞和去抖得到间隙与状态；
6. 每 100 ms 通过 USART2 上报。

LDC1614 当前配置：

| 参数 | 值 |
| --- | --- |
| RCOUNT | `0xFFFF` |
| SETTLECOUNT | `0x0180` |
| CLOCK_DIV | `0x1001` |
| DRIVE_CURRENT | `0x9000` |
| MUX | CH0 + CH1 自动扫描 |
| CONFIG | 优先外部：`2A01` → `0A01`；启动失败一次内部对照：`2801` → `0801` |
| ERROR_CONFIG | `0x0001`，使能 DRDY → INTB |

## 七、温度与 Flash 参数

内部温度使用 ADC1 温度通道、VREFINT、`TS_CAL1` 和
`VREFINT_CAL` 估算，结果为 MCU 芯片自身温度。

STM32C011F6 的 32 KB Flash 页大小为 2 KB。参数区位于：

```text
FLASH_PARAM_ADDR = 0x08007800
FLASH_PARAM_SIZE = 2048
```

当前默认不从 Flash 加载参数，但串口 `CALS` / `CLR` 命令仍会调用参数写入。
V12 最终链接和 HEX 校验通过，镜像末地址（不含）为 `0x08007760`，
距离 `0x08007800` 参数页还有 160 字节，不与参数区重叠，余量已很小。
后续扩展仍需重新检查程序与参数区不重叠。

## 八、常见问题

| 现象 | 优先检查 |
| --- | --- |
| 串口无输出 | PA2 接 CH340 RXD、PA3 接 TXD、共地、115200 8N1 |
| `CLOCK_UNEXPECTED` | PC14/PC15 8 MHz 晶振、负载电容和焊接 |
| `$E,10` / `$E,11` | 先比较 REF_ID / IRQ_ID / PRE_ID，区分读 ID 与后续配置失败 |
| `$E,17` | 看 `$C/$Q`：两种参考下唤醒是否失败、其他配置是否恢复默认；不是缺 INTB |
| `$E,18` | 看 `$K,...,HOLD` 和 `$Q`：配置是否丢失/不匹配，不能因 DATA 正常忽略 |
| `$E,20` | 配置通过但无两组有效新转换，检查 `$V`、参考时钟/线圈及数据错误 |
| `LDC_DIAG_ONLY` | 未通过完整测量；仍每秒只读 ID/配置/状态/原始数据，不自动复位，不报测量正常 |
| `LDC_CLOCK,INTERNAL` | 内部参考对照成功；检查外部 40 MHz 路径，并核验距离精度 |
| 状态一直为 3 | 先看启动诊断；运行阶段检查是否持续产生新转换或总线是否出错 |
| 状态一直为 4 | 标定点和标定表 |
| 距离不可信 | 更换线圈或板卡后重新标定 |
