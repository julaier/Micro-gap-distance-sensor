# LDC1614 微缝隙测量固件 (STM32F103C8T6 + OLED)

电梯曳引机制动器松闸间隙测量，LDC1614 双通道差分方案。
本工程是**新验证板**的固件：串口用 USART1，I2C 总线上多挂了一块 OLED，PA0/PA1 两个按键。

工程目录：`D:\Codex_Workspace\new`
Keil 工程：`MDK-ARM\LDC1614.uvprojx`

---

## 一、目录结构

```
new/
  App/
    main.c                 入口, 只负责调用 app
    app.c / app.h          初始化顺序、INTB 采样、按键、周期上报
    display.c / display.h  OLED 显示层: 主界面/调试界面布局与刷新
  Driver/
    board.c / board.h         系统时钟(先 HSI, 再试外部 8MHz)、1ms 时基、INTB(PB0/EXTI0)
    hard_i2c.c / hard_i2c.h   STM32 硬件 I2C1 (PB6=SCL, PB7=SDA)，LDC 和 OLED 共用
    uart.c / uart.h           USART1 (PA9=TX, PA10=RX)，接外部 CH340
    oled.c / oled.h           SSD1306 128x64 OLED 驱动 (HS96L03W2C03)
    button.c / button.h       PA0 / PA1 按键，20ms 消抖，支持短按/长按
    adc_temp.c / adc_temp.h   MCU 内部温度 (ADC1_IN16 + VREFINT 校准)
    flash_param.c / flash_param.h  内部 Flash 参数区 (当前不使用)
  Device/
    ldc1614.c / ldc1614.h     LDC1614 寄存器读写、初始化、读双通道
  Service/
    measure.c / measure.h     滤波、标定插值、温补、迟滞+去抖
    protocol.c / protocol.h   串口帧组装与命令解析(命令解析未启用)
  CMSIS/  Library/  Startup/   STM32F103 标准库和启动文件
  MDK-ARM/                    Keil 工程
  tools/keil_build.cmd        命令行编译/烧录脚本
```

依赖方向：`App -> Service -> Device -> Driver`。

---

## 二、硬件约定

| 项 | 接法 |
| --- | --- |
| 主控 | STM32F103C8T6 |
| STM32 时钟 | **内部 HSI 8MHz（不用 HSE/PLL）** |
| LDC1614 I2C | 硬件 I2C1，PB6 = SCL，PB7 = SDA，**100kHz**，板上 4.7K 上拉 |
| OLED | HS96L03W2C03（0.96 寸 SSD1306 兼容，128x64，I2C），**与 LDC 共用 PB6/PB7** |
| OLED 地址 | 0x3C / 0x3D，程序上电自动试 |
| LDC1614 INTB | PB0 / EXTI0，下降沿触发 |
| LDC1614 ADDR | GND -> 7 位地址 0x2A |
| LDC1614 SD | GND -> 正常工作 |
| LDC1614 CLKIN | 外部 40MHz 有源晶振（单端） |
| 传感线圈 | 板载两路 PCB 平面线圈：CH0 = 测量，CH1 = 参考 |
| 未用通道 | IN2A/IN2B/IN3A/IN3B 按硬件设计接 GND |
| 串口 | USART1，**PA9 = TX，PA10 = RX**，115200 8N1，接外部 CH340 |
| 按键 1 | PA0，按下为低电平（内部上拉） |
| 按键 2 | PA1，按下为低电平（内部上拉） |
| 温度 | STM32 内部温度通道（ADC1_IN16），仅作临时温补 |

---

## 三、Keil 编译 / 下载

1. 双击 `MDK-ARM\LDC1614.uvprojx`。
2. 器件 `STM32F103C8`，编译器 ARMCC5，宏 `STM32F10X_MD;USE_STDPERIPH_DRIVER`。
3. **F7** 编译，输出 `0 Error(s), 0 Warning(s)`。
4. **F8** 下载（ST-Link，SWD），然后按复位。

命令行等价（工程根目录）：

```bat
tools\keil_build.cmd build      rem 增量编译
tools\keil_build.cmd rebuild    rem 全部重编
tools\keil_build.cmd flash      rem 编译 + ST-Link 下载
```

> 注意：如果 Keil 一直开着工程，代码被外部修改后要先 **File → Reload**，别直接保存旧缓存。

---

## 四、上电启动信息

串口（PA9/PA10，115200 8N1）会依次打印：

```
$B,BOOT,HSI8M      ← 内部 HSI 8MHz（本版固定配置）
$B,OLED OK         ← OLED 应答正常；OLED FAIL 表示没找到屏幕
$B,LDC1614 ready   ← LDC1614 初始化成功
$R,<ch0>,<ch1>,<比值>,<距离mm>,<温度C>,<状态>
...
```

初始化失败时：

```
$E,<code>          ← 错误码见下表
$I,<pd>,<pu>       ← I2C 总线电平诊断（仅失败时）
$P,<addr>,<ack>    ← 0x2A~0x2D 地址探测（仅失败时）
```

| 错误码 | 含义 |
|---:|---|
| 10 | 软复位失败 |
| 11 | ID 读取失败或 ID 不匹配 |
| 12 | 时钟分频配置失败 |
| 13 | SETTLECOUNT 配置失败 |
| 14 | RCOUNT 配置失败 |
| 15 | 驱动电流配置失败 |
| 16 | MUX 配置失败 |
| 17 | CONFIG 配置失败 |
| 18 | CONFIG 回读校验失败 |

温度明显异常时，会额外打印一行（正常时不打印）：

```
$B,TEMP,cal=...,vref=...,ts=...,t10=...
```

---

## 五、串口协议

### 5.1 周期上报（100ms 一帧）

```
$R,<ch0>,<ch1>,<比值>,<距离mm>,<温度C>,<状态>\r\n
```

例：`$R,11284266,11580266,0.97443,0.520,28.5,1`

状态：`0` 正常 / `1` 提醒 / `2` 报警 / `3` 故障 / `4` 未标定。

### 5.2 串口接收

PA10 已经配置为 USART1_RX，底层提供 `uart_read_byte()`；
但当前 app 没有接命令解析，**电脑发命令暂时不会响应**。
`protocol.c` 里的命令解析代码保留，后续需要接收命令时再接上。

### 5.3 精简帧

`$D,<距离mm>,<温度C>,<状态>` 的组装函数保留，当前周期上报只用 `$R`。

---

## 六、OLED 显示

### 主界面

```
GAP          NORMAL      ← 状态: NORMAL/WARN/ALARM/FAULT/UNCAL
   0.520 mm              ← 距离(大字, 0.001mm 分辨率)
T 28.5 C                 ← 温度
R 1.01027                ← 滤波后的比值
```

未标定时距离显示 `--.--`，状态显示 `UNCAL`。

### 调试界面（按 KEY1 切换）

```
DEBUG
CH0 11284266
CH1 11580266
RATIO 1.01027
TEMP 28.5 C
ST NORMAL
```

### 显示参数

| 参数 | 位置 | 当前值 |
| --- | --- | --- |
| 刷新周期 | `App/app.c` | `APP_OLED_MS = 250ms` |
| I2C 地址 | `Driver/oled.c` | 自动试 0x3C / 0x3D |
| 分辨率 | `Driver/oled.h` | 128 x 64 |

---

## 七、按键

| 按键 | 引脚 | 功能 |
| --- | --- | --- |
| KEY1 | PA0 | **短按：主界面 ↔ 调试界面** |
| KEY2 | PA1 | **短按：冻结/恢复显示（HOLD）** |

- 按下为低电平，内部上拉，20ms 消抖。
- 驱动还支持长按 2 秒事件（`button_long_press()`），当前 app 未使用，预留给"零点标定"等功能。

---

## 八、采样与处理流程

1. LDC1614 自动扫描 CH0 + CH1，每完成一轮转换 INTB 拉低 → PB0 触发 EXTI0；
2. 主循环在 `board_intb_take()` 返回 1 时读 DATA_CH0 / DATA_CH1（28 位原始值）；
3. 比值（默认 CH0/CH1）→ 5 点中值 → 8 点滑动平均；
4. 温度修正 → 标定表分段线性插值得到间隙；
5. 阈值判断（0.05mm 迟滞）+ 1.5s 去抖 → 状态码；
6. 每 100ms 发一帧 `$R`；超过 **500ms** 没有 INTB 或 I2C 读失败 → 状态 3（故障）。

RCOUNT 当前 `0xFFFF`（约 26ms/通道，双通道一轮约 52ms），上报 100ms 一帧。
要提速时改 `Device/ldc1614.c` 的 `LDC1614_RCOUNT_VALUE`。

---

## 九、关键寄存器配置

都在 `Device/ldc1614.c` 顶部：

| 常量 | 当前值 | 含义 |
| --- | --- | --- |
| `LDC1614_RCOUNT_VALUE` | `0xFFFF` | 转换时间，约 26ms/通道 @40MHz |
| `LDC1614_SETTLECOUNT_VALUE` | `0x0180` | 建立时间 |
| `LDC1614_CLOCK_DIV_DEFAULT` | `0x1001` | 时钟分频 |
| `LDC1614_DRIVE_CURRENT_VALUE` | `0x9000` | 驱动电流初值 |
| `LDC1614_MUX_AUTOSCAN_CH01` | `0x820C` | 自动扫描 CH0+CH1（复位默认 0x020C + bit15） |
| `LDC1614_CONFIG_REF_CLK_SRC` | `0x0200` | **置 1 = 用外部 40MHz CLKIN** |
| `LDC1614_CONFIG_SLEEP_MODE` | `0x2000` | 初始化时清零，退出睡眠 |

初始化会校验 `MANUFACTURER_ID = 0x5449`、`DEVICE_ID = 0x3055`，
不对会报 `$E,11`。I2C 的 2 字节读采用"逐字节收"方式（已验证可靠）。

---

## 十、参数在哪里改

| 要改什么 | 文件 | 宏 / 变量 |
| --- | --- | --- |
| 波特率 | `App/app.c` | `APP_BAUD` |
| 上报周期 | `App/app.c` | `APP_REPORT_MS` |
| 读温度间隔 | `App/app.c` | `APP_TEMP_MS` |
| INTB 超时 | `App/app.c` | `APP_INTB_TIMEOUT` |
| OLED 刷新周期 | `App/app.c` | `APP_OLED_MS` |
| 报警/提醒/迟滞 | `Service/measure.c` | `MEAS_DEFAULT_ALARM_MM`、`MEAS_DEFAULT_WARN_MM`、`MEAS_DEFAULT_HYST_MM` |
| 温补系数/参考温度 | `Service/measure.c` | `MEAS_DEFAULT_TEMP_K`、`MEAS_DEFAULT_TEMP_REF_C` |
| 通道交换 | `Service/measure.c` | `MEAS_DEFAULT_SWAP` |
| 标定点个数 | `Service/measure.c` | `MEAS_CODE_CAL_POINTS` |
| 标定表 | `Service/measure.c` | `s_code_cal_gap[]`、`s_code_cal_ratio[]` |
| RCOUNT / 驱动电流等 | `Device/ldc1614.c` | `LDC1614_RCOUNT_VALUE` 等 |
| 看门狗 | `Driver/board.c` | `BOARD_USE_IWDG`（当前 0 = 关闭） |

> **注意**：当前标定表还是**旧板**测的 4 个点
> （0.5mm→1.01027，0.8mm→1.00940，1.0mm→1.00517，2.0mm→0.99091），
> 换板/换线圈后**必须重新标定**，标定前距离数字不要当真。

---

## 十一、温度

- 使用 STM32 内部温度传感器（ADC1_IN16）+ VREFINT 校准；
- 公式：`VDD = 3.3V × VREFINT_CAL / VREFINT_RAW`，`Vsense = VDD × ADC / 4096`，
  `T = (1.43V - Vsense) / 4.3mV + 25`；
- 结果限幅在 -40 ~ 125°C；
- 测的是 **MCU 芯片自身温度**，不是线圈/目标温度，只能做临时温补。

---

## 十二、常见问题

| 现象 | 优先检查 |
| --- | --- |
| 串口无输出 | COM 口、PA9 是否接 CH340 的 RXD、GND 共地、115200 |
| `$B,BOOT,CLOCK_UNEXPECTED` | 时钟不是 HSI 8MHz，检查 system_stm32f10x.c / board.c |
| `$B,OLED FAIL` | OLED 地址不对/没接/供电；可查 0x3C、0x3D，或确认是否 SH1106 |
| OLED 花屏/偏 2 列 | 可能不是 SSD1306 而是 SH1106，需要改初始化 |
| `$E,10` | 软复位失败：I2C 线、供电、ADDR |
| `$E,11` | ID 读取失败：I2C、器件供电、型号 |
| `$E,10` 但 OLED OK | LDC1614 没上总线：查 VDD、SD、SCL/SDA 焊接与方向 |
| 状态一直 3 | INTB 没来：40MHz 时钟、CONFIG、MUX、PB0 |
| 状态一直 4 | 未标定：`MEAS_CODE_CAL_POINTS` 是否 ≥ 2，标定表是否填 |
| 距离一直是 0 | 同上（未标定） |
| 温度异常 | 看 `$B,TEMP,...` 原始值 |
