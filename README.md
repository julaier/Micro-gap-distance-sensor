# LDC1614 微缝隙测量固件 (STM32F103C8T6)

电梯曳引机制动器松闸间隙测量，LDC1614 双通道差分方案。
本工程是 **验证板 Board1** 的固件，一份代码同时给 Keil 和 VS Code 用。

---

## 一、目录结构

```
pa2/                      （从 LDC1614_FW 复制，串口版调试工程）
  App/                   应用层
    main.c                 入口, 只负责调用 app
    app.c / app.h          初始化顺序、采样、命令、周期上报
  Driver/                与芯片/板级硬件打交道的驱动
    board.c / board.h        时钟 72MHz、1ms 时基、INTB(PB0/EXTI0)
    hard_i2c.c / hard_i2c.h  STM32 硬件 I2C1 (PB6=SCL, PB7=SDA)
    uart.c / uart.h          USART2 (PA2=TX, 只发不收), 接外部 CH340 模块
    adc_temp.c / adc_temp.h  内部温度 (ADC1 通道 16 + VREFINT 校准)
    flash_param.c / flash_param.h  内部 Flash 参数区 (0x0800FC00, 1KB)
  Device/                器件驱动
    ldc1614.c / ldc1614.h    LDC1614 寄存器读写、初始化、读双通道
  Service/               与硬件无关的服务
    measure.c / measure.h    滤波、标定插值、温补、迟滞+去抖
    protocol.c / protocol.h  串口帧组装与命令解析
  CMSIS/                 STM32 CMSIS 内核与器件寄存器定义
  Library/
    inc/                    STM32F10x SPL V3.5.0 头文件
    src/                    STM32F10x SPL V3.5.0 源文件
  Startup/               厂商文件 (启动文件)
  MDK-ARM/               Keil uVision5 工程
    LDC1614.uvprojx         工程文件, 双击即可用 Keil 打开
    LDC1614.uvoptx          调试/下载设置 (已选 ST-Link, SWD)
  tools/keil_build.cmd   VS Code 任务用的命令行构建脚本
  .vscode/               VS Code 工程配置
```

依赖方向是单向的：`App -> Service -> Device -> Driver`。
`Service` 层不访问寄存器，`Driver` 层不含业务逻辑。

---

## 二、硬件约定

| 项 | 接法 |
| --- | --- |
| 主控 | STM32F103C8T6, HSE 8MHz, 系统 72MHz |
| LDC1614 I2C | 硬件 I2C1，PB6 = SCL，PB7 = SDA，400kHz，板上 4.7K 上拉 |
| LDC1614 INTB | PB0 / EXTI0，下降沿触发（INTB 为推挽，无需上拉） |
| LDC1614 ADDR | GND -> 7 位地址 0x2A |
| LDC1614 SD | GND -> 正常工作 |
| LDC1614 CLKIN | 外部 40MHz 有源晶振（单端） |
| 传感线圈 | 板载两路 PCB 平面线圈，接 IN0A/IN0B 与 IN1A/IN1B |
| 未用通道 | IN2A/IN2B/IN3A/IN3B 并接 GND，扫描里不使用 |
| 上位机串口 | USART2，PA2 = TX（只发不收），115200 8N1，接外部 CH340 模块 |
| USB | 仅取电（D+/D- 未接），调试板由 Micro-USB 5V 供电 |
| 温度 | STM32 内部温度通道，仅作临时温补 |

---

## 三、用 Keil 编译 / 下载

1. 双击 `MDK-ARM\LDC1614.uvprojx`，用 Keil uVision5 打开。
2. 器件已设为 `STMicroelectronics -> STM32F103C8`，编译器 ARMCC5。
   工程已定义 `USE_STDPERIPH_DRIVER`，并在 `Library` 分组中编译标准外设库；
   应用层和驱动层统一调用 SPL API。
3. 按 `F7` 编译，生成的固件在 `MDK-ARM\Objects\LDC1614.hex`。
4. 按 `F8` 下载（Debug/Utilities 已设为 ST-Link Debugger，接口 SWD）。

命令行等价（在工程根目录）：

```bat
tools\keil_build.cmd build      rem 增量编译
tools\keil_build.cmd rebuild    rem 全部重编
tools\keil_build.cmd flash      rem 编译 + ST-Link 下载
```

> 如果 Keil 装在别的位置，改 `tools\keil_build.cmd` 第一行的 `UV4=` 即可。

## 四、用 VS Code 编辑

用 VS Code 打开 `LDC1614_FW` 文件夹（或工作区文件）。`Ctrl+Shift+B` 触发
`Keil: 构建 (Build)`，任务会调用 `tools\keil_build.cmd` 调 UV4 编译，
出错信息会解析到 `问题` 面板。

`.vscode/c_cpp_properties.json` 已配好头文件路径与 `STM32F10X_MD` 宏，
IntelliSense 可直接跳转。

---

## 五、串口协议

### 5.1 周期上报（默认 100ms 一条，115200 8N1）

本工程单片机的串口只发送、不接收，所以周期上报直接发 `$R` 原始帧：一条里同时带原始值、
比值、距离、温度和状态，MobaXterm 直接看这一行就行。
```
$R,<ch0>,<ch1>,<比值>,<间隙mm>,<温度C>,<状态>\r\n
```

例：`$R,3953210,3948021,1.00131,0.523,28.5,0`

状态：`0` 正常 / `1` 提醒 / `2` 报警 / `3` 故障 / `4` 未标定。

标定时把 `比值` 字段抄下来，按间隙从小到大填进 `Service/measure.c` 的代码标定表。

### 5.2 精简帧（保留，当前周期上报不用）

```
$D,<间隙mm>,<温度C>,<状态>\r\n
```

只要距离和状态时可用，`protocol_build_d()` 里的代码没删，以后接上位机还能用。

### 5.3 上电提示

- 正常：`$B,LDC1614 ready`
- 初始化失败：`$E,<code>`，code 为负数取绝对值（见 `Device/ldc1614.c` 的 `LDC_ERR_xxx`）

### 5.4 上位机命令（每条以 `\r\n` 结尾）

单片机的 PA2 只发送，电脑的命令进不来，所以下面这些命令在本工程暂时用不上；
`protocol.c` 里的解析代码保留，以后接上接收线再启用。所有参数直接改代码（见第八节）。

| 命令 | 功能 |
| --- | --- |
| `READ` | 立刻回一帧 `$D` |
| `RAW` | 立刻回一帧 `$R` |
| `RATE,100` | 上报周期 ms，`0` = 停止主动上报 |
| `STX,0.800` | 设置报警阈值（mm） |
| `ST2,0.500` | 设置提醒阈值（mm） |
| `SWAP,1` | 交换 CH0/CH1 的比值方向（哪个是测量线圈不确定时用） |
| `CAL,1,0.300` | 用当前比值录入第 1 个标定点 = 0.300mm（必须从 1 依次录入） |
| `CALS` | 标定表与参数写入 Flash |
| `CLR` | 清空标定表 |
| `TC,0.00002,25.0` | 温补系数与参考温度 |
| `INFO` | 返回版本、周期、阈值、标定点、当前值 |
| `RD,16` | 读寄存器（十六进制），返回 `REG,<reg>,<val>` |
| `WR,17,820F` | 写寄存器 |
| `DUMP` | 一次打印 0x00~0x1E 全部寄存器的低字节 |

应答：`OK` / `ERR,<code>`。

---

## 六、采样与处理流程

1. LDC1614 按 CH0 + CH1 自动扫描，每完成一次转换 INTB 拉低，PB0 触发 EXTI0；
2. 主循环读 DATA_CH0 / DATA_CH1 的 28 位原始值（MSB 高 4 位是错误标志）；
3. 比值（默认 ch0/ch1，可用 `SWAP` 翻转）-> 5 点中值 -> 8 点滑动平均；
4. 温度二阶修正 -> 标定表分段线性插值得到间隙；
5. 阈值判断（0.05mm 迟滞）+ 1.5s 去抖 -> 状态码；
6. 每 100ms 发一帧 `$R`；超过 500ms 没有 INTB 或 I2C 读失败 -> 状态 3（故障）。

RCOUNT 当前取 `0xFFFF`（约 26.2ms/通道，双通道一轮约 52ms），
所以上报周期不要小于 60ms。台架上要提速时改 `Device/ldc1614.c` 里的
`LDC1614_RCOUNT_VALUE`。

---

## 七、上板前必须核对的寄存器常量

这几个值按数据手册填写，**投产前请对照手头的 LDC1612/1614 数据手册再确认一遍**，
都在 `Device/ldc1614.c` 顶部：

| 常量 | 当前值 | 含义 |
| --- | --- | --- |
| `LDC1614_RCOUNT_VALUE` | `0xFFFF` | 转换时间，约 26.2ms/通道 @40MHz |
| `LDC1614_SETTLECOUNT_VALUE` | `0x0180` | 建立时间，需 >= (fREF/fSENSOR) x Q |
| `LDC1614_CLOCK_DIV_DEFAULT` | `0x1001` | 时钟分频 |
| `LDC1614_DRIVE_CURRENT_VALUE` | `0x9000` | 驱动电流初值 |
| `LDC1614_MUX_AUTOSCAN_CH01` | `0x820F` | 自动扫描 CH0+CH1 |
| `LDC1614_CONFIG_REF_CLK_SRC` | `0x0200` | CONFIG 的 REF_CLK_SRC，用外部时钟 |
| `LDC1614_CONFIG_SLEEP_MODE` | `0x2000` | CONFIG 的 SLEEP_MODE_EN，初始化时清零 |

其中 `MUX_AUTOSCAN_CH01` 和 `CONFIG_REF_CLK_SRC` 最关键：配错的话上电
INTB 不来，串口只会发 `$R,xx,xx,xx,0.000,xx,4` 或直接判故障。
本工程只发送，不能在线发 `DUMP` / `WR`；要核对寄存器就临时接上接收线启用命令解析，
或用 Keil 调试器直接看。寄存器常量改完重新编译烧录。

寄存器地址按 TI `SNOSCY9A`（LDC1612/LDC1614 Rev. A）实现：
`CONFIG=0x1A`、`MUX_CONFIG=0x1B`、`RESET_DEV=0x1C`、
`DRIVE_CURRENT0/1=0x1E/0x1F`、`MANUFACTURER_ID=0x7E`、`DEVICE_ID=0x7F`。
初始化会先校验 `0x5449/0x3055`，ID 不对会通过串口报 `E,11`，不会假报 ready。

---

## 八、参数在哪里改（本工程串口只发送）

调参不用连电脑，改完代码编译烧录即可。

| 要改什么 | 文件 | 宏 / 变量 |
| --- | --- | --- |
| 波特率 | `App/app.c` | `APP_BAUD` |
| 上报周期 | `App/app.c` | `APP_REPORT_MS` |
| 读温度间隔 | `App/app.c` | `APP_TEMP_MS` |
| INTB 超时 | `App/app.c` | `APP_INTB_TIMEOUT` |
| 是否用 Flash 旧参数 | `Service/measure.c` | `MEAS_USE_FLASH_PARAMS`（当前 0 = 不用） |
| 报警/提醒/迟滞 | `Service/measure.c` | `MEAS_DEFAULT_ALARM_MM`、`MEAS_DEFAULT_WARN_MM`、`MEAS_DEFAULT_HYST_MM` |
| 温补系数/参考温度 | `Service/measure.c` | `MEAS_DEFAULT_TEMP_K`、`MEAS_DEFAULT_TEMP_REF_C` |
| 通道交换 | `Service/measure.c` | `MEAS_DEFAULT_SWAP` |
| 标定点个数 | `Service/measure.c` | `MEAS_CODE_CAL_POINTS`（至少 2 点，未填时距离为 0、状态 4） |
| 标定表 | `Service/measure.c` | `s_code_cal_gap[]` 与 `s_code_cal_ratio[]`，按间隙从小到大成对填写 |
| RCOUNT / 驱动电流等寄存器常量 | `Device/ldc1614.c` | `LDC1614_RCOUNT_VALUE` 等 |
