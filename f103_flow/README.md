# C011 软件 I²C：原 F103 初始化流程验证版

2026-11-14 新增独立 V9 时序留余量版：打开
`MDK-ARM/LDC1614_F103_Margin_STLink.uvprojx`，使用说明见 `README_Margin.md`。
以下内容仍描述保留的 V8 对照工程。

固定原接线：**物理 PB7=SCL、PB6=SDA**，PA2=TX、PA3=RX、PA4=INTB 输入。
这是独立的通信、原始配置和原始数据验证工程，**不是完整的距离测量应用**。
不覆盖正式程序、原 F103 程序、test、wake_test 或 A/B/C 对照工程。

## 直接使用

1. Keil5 打开 `MDK-ARM/LDC1614_F103_Flow.uvprojx`。
2. 只有一个 Target：`LDC1614_F103_FLOW_SW`，无需再选择 A/B/C 或改宏。
3. Rebuild、手动 Download，退出 Debug 连续运行。串口 115200、8N1。
4. 保存从 `$B,FW` 到 `$B,RESULT` 的完整启动日志。

新版本标识必须为：

```text
$B,FW,C011_LDC_F103_FLOW_V8_SW
$B,PROFILE,B_OD3_UNCHANGED
$B,CONFIG,F103_ORIGINAL_FLOW,EXT40,NO_EXTRA_SLEEP_WRITE
```

HEX：`MDK-ARM/Objects/LDC1614_F103_FLOW_SW.hex`。
脚本 `tools/keil_build.cmd` **只编译，不烧录、不复位、不改选项字节**。

### ST-Link 下载设置（2026-11-14 补全）

为避开已打开工程中的旧 ULINK 缓存，可直接打开专用入口
`MDK-ARM/LDC1614_F103_Flow_STLink.uvprojx`；它附带同名 ST-Link 选项文件，
目标、源代码及输出 HEX 与主入口相同，无需修改代码或再次选择 A/B/C。

新工程最初未带 `.uvoptx`，Keil 自动生成了 ULINK2 默认设置，
会出现 `No ULINK2/ME Device found`。现已补齐 ST-Link 选项文件：

- Debug 选择 `ST-Link Debugger`，非模拟器或 ULINK；
- Utilities 复用 Debug 驱动，同时下载驱动也指定为 ST-Link；
- 连接参数沿用本机此前 C011 工程，SWD 速率设置为 1000 kHz；
- Flash 算法为设备包的 `STM32C0x_32.FLM`，Flash 起始 `0x08000000`、32 KB；
- 修改前选项备份为 `MDK-ARM/LDC1614_F103_Flow.uvoptx.before_stlink.bak`。

若 Keil 正开着修改前工程，内存中仍是旧 ULINK 设置：关闭该工程，
**不要保存旧的工程选项**，然后重新打开同一个 `.uvprojx` 再 Download。
若仍显示 ULINK，在 `Options for Target → Debug → Use` 选择 `ST-Link Debugger`，
并在 `Utilities` 勾选 `Use Debug Driver`，点击 OK 保存。
这次只修正主机工程设置，没有实际下载、擦除、复位或修改芯片选项字节。

## 与原 F103 一致的部分

`Device/ldc1614.c` 完整复用 `../../new/Device/ldc1614.c`，代码未改；
测试脚本逐字比较（忽略文件换行形式与末尾空白）。`.h` 只修正本板接线说明。
底层通过 `Driver/f103_i2c_adapter.c` 适配到已有 B 软件 I²C，
`hard_i2c_*` 只是为了兼容旧源文件而保留的函数名，**不是硬件 I²C**。
不编译 F103 外设驱动，不使用 HAL。

实际初始化事务顺序：

| 顺序 | 操作 |
| --- | --- |
| 1 | 写 `1C=8000`，等待 5 ms |
| 2 | 读取并确认 `7E=5449`、`7F=3055` |
| 3 | 写 `14/15=1001` |
| 4 | 写 `10/11=0180` |
| 5 | 写 `08/09=FFFF` |
| 6 | 写 `1E/1F=9000` |
| 7 | 写 `1B=820C` |
| 8 | 读 CONFIG，执行原算法 `(v & ~(2000|0200)) | 0200`，写 CONFIG |
| 9 | 立即读 CONFIG，按原 F103 条件验证外部参考、已退出休眠 |

CONFIG 复位读值为 `2801` 时，上述算法得到 `0A01`。
没有额外的 `2A01` 写入、1 秒睡眠保持、逐寄存器交错回读、
ERROR_CONFIG 写入、LDC 内部参考切换、失败自动复位或重试。
原初始化函数内不插入串口输出，事务摘要缓存在 RAM，初始化结束后才打印。
后续完整快照属于**初始化结束后的诊断**，不改变上面的原配置流程。

## 仍不相同的部分与限制

- 原 F103 是硬件 I²C，配置 100 kHz；这里复用 `ack_compare` 的 B 版软件 I²C，
  **没有改变 B 的 GPIO、数据保持或三点 ACK 采样路径**。请求等待 5 µs 另有执行开销，
  之前软件估算约 36 kHz，不能据此声称实际波形与 F103 完全一致或精确 100 kHz。
- 当前 C011 使用 HSE 8 MHz 直接运行；时钟失败时板级代码会报告 HSI_FALLBACK。
  这是主控启动时钟处理，不是 LDC 参考时钟回退。LDC 始终选择外部参考。
- PA4 不启用中断，未启动 OLED、ADC、按键服务、SysTick 或 MCU 看门狗。
  原工程中 OLED/按键源码及注释保持原样。
- 初始化成功后每次等待 20 ms 再轮询 STATUS（通信与打印还占用执行时间）。
  仅两路 UNREAD 标志均置位才读原始数据，不依赖 INTB。
- CH0/CH1 未安装线圈时，配置通信通过也**不代表正常转换或距离测量通过**。
  此工程没有温度补偿、标定表、距离算法或原应用 `$R` 上报；先验证原流程配置能否成功。
- 代码已配置正确、ID 可读，都不能据此排除实物供电、SD、上拉、焊接或波形问题。

## 日志与 Keil Watch

- `$F,STEP,操作,寄存器,返回码,值,总线阶段,数据索引,最后发送字节,STOP返回码,软件估算Hz`
  为原初始化的按序记录；R 的值只在返回码 0 时有效。
- `$F,FIRST` 保存原初始化里首个失败事务，之后监控不覆盖。
  `$A,FIRST,字节槽,发送字节,采样数,SDA掩码,SCL掩码,返回码` 是该事务的 B 版 ACK 证据。
  SDA=0/SCL=7 为三点均 ACK；SDA=7/SCL=7 为三点均 NACK；混合不会被放行。
- `$B,ORIGINAL_INIT_RC` 是原 F103 驱动的初始化返回码，例如 -17=CONFIG阶段失败；
  `$F,FIRST` 中的 -2 才是底层 NACK。两者不要混为 LDC 芯片自身故障码。
- `$B,RESULT,CONFIG_PASS_NOT_MEASUREMENT,0` 仅表示初始化及随后配置读取通过。
- 初始化失败时，每秒只读快照，`HOLD=-4` 表示原初始化未完成，不能据此判断“自发复位”。
  本工程开始就主动执行过一次软件复位，随后中止时出现默认值可以是预期结果。
- 初始化通过时，`$POLL,rc,STATUS,CH0,CH1,DATA错误,新数据对累计,有效数据对累计`：
  rc=0 是本轮读到非零、无已报告 DATA 错误的新双通道数据；1=暂未两路就绪；
  2=读到双通道数据但为零或有 DATA 错误；负数=读取失败。**不等于距离标定已通过**。

Watch 可添加 `g_flow_debug`、`g_flow_bus`。安全断点为 `f103_flow_done_checkpoint`。
不要在 I²C 位传输中单步/暂停，否则会改变时序。

## 本机验证

```powershell
.\tools\run_tests.ps1
..\ack_compare\tools\run_tests.ps1
cmd /c tools\keil_build.cmd
```

新测试执行实际原 F103 LDC 源码、适配层和启动代码，核对 15 笔原初始化事务顺序，
逐笔注入 NACK，检查最后字节 NACK 即使已写入也不冒充成功、默认值恢复、ID错误、
初始总线错误、回读失败、首次错误保存、无额外延时/睡眠/重试、数据新鲜度及错误标志。
已有 A/B/C 回归测试覆盖实际软件底层的数字边沿、ACK与STOP；两者均不能代替实物波形验证。
