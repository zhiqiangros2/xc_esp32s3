# 正点原子 DNESP32S3 BOX3 板级功能示例

这是一个基于 ESP-IDF 5.5 的最小示例工程，适用于正点原子
`ATK-DNESP32S3B3 V1`（BOX3）开发板。

程序启动后会检测 Flash 和 PSRAM 容量，执行一次 1 MiB PSRAM 写入/读回测试，
初始化 AW9523B 扩展 GPIO、板载 2.4 英寸 LCD 和外接 CHSC5432 电容触摸屏，
在屏幕上显示测试色条，启用 K0/K1/K2 与触摸中断，然后每隔五秒通过 USB
串口输出一条运行日志。

## 硬件配置

| 项目 | 配置 |
| --- | --- |
| 主控 | ESP32-S3R8，双核 Xtensa LX7 |
| PSRAM | 芯片内置 8 MiB Octal PSRAM，80 MHz |
| Flash | BY25Q128ES，16 MiB Quad SPI，80 MHz |
| K0 按键 | GPIO0，上拉输入，按下为低电平 |
| AW9523B | I2C0，SCL=GPIO2，SDA=GPIO3，地址 `0x59`，INT=GPIO42 |
| LCD | ST7789V2，320×240，SPI2：SCLK=GPIO15、MOSI=GPIO16、MISO=GPIO17、CS=GPIO47、DC=GPIO48；RESX 与 CHIP_PU 共用复位网络 |
| LCD 背光 | AW9523B P1_0，低电平点亮 |
| 外接触摸屏 | CHSC5432，I2C 地址 `0x2E`，INT 与 AW9523B 共用 GPIO42，RESET=AW9523B P1_7 |
| 下载与日志 | Type-C 原生 USB，GPIO19/GPIO20 |
| 开发板型号 | ATK-DNESP32S3B3 V1 |

配置依据见 `pdf` 目录中的
[ATK_DNESP32S3B3_V1.0.pdf](./pdf/ATK_DNESP32S3B3_V1.0.pdf) 和
[AW9523BTQR.pdf](./pdf/AW9523BTQR.pdf)，LCD 控制器配置另参考
[ST7789.pdf](./pdf/ST7789.pdf)、[CHSC5xxx.pdf](./pdf/CHSC5xxx.pdf)、
[正点原子 BOX3 SPI-LCD 实验](https://wiki.alientek.com/docs/Boards/IoT/DNESP32S3B3/example-idf/lcd/)
和[正点原子 BOX3 触摸实验](https://wiki.alientek.com/docs/Boards/IoT/DNESP32S3B3/example-idf/touch/)。

板载红蓝双色 LED 并未直接连接 ESP32-S3 GPIO，而是连接在 AW9523B IO
扩展芯片的 P1_1 和 P1_2 上。本示例初始化 AW9523B、校验芯片 ID、配置端口
方向，并将红灯和蓝灯设置为熄灭状态。

## 工程功能

- 输出 ESP-IDF 版本、芯片型号、CPU 核心数和芯片版本。
- 检测实际 Flash 容量，期望值为 16 MiB。
- 检测实际 PSRAM 容量，期望值为 8 MiB。
- 从 PSRAM 分配 1 MiB 缓冲区并完成写入、读回校验。
- 初始化 AW9523B 独立驱动，并验证 ID 寄存器值为 `0x23`。
- 初始化 ST7789V2 LCD，以左下角为原点、Y 为物理横轴显示八色测试图。
- 初始化 CHSC5432，读取芯片 ID，并通过 GPIO42 中断输出最多 5 个触点坐标。
- 通过 GPIO42 共享中断管理器查询 AW9523B；K1/K2 分别切换红灯/蓝灯。
- GPIO42 使用标志位选择处理模块；AW9523B 对应 `0x0001`，触摸对应 `0x0002`。
- 使用 GPIO0 下降沿中断检测 K0，任务中进行 20 ms 消抖；每次按下打印一次日志。
- 每隔五秒输出运行时间，便于确认程序持续运行。

### AW9523B 驱动

`i2c.c/.h` 封装 ESP-IDF 5.5 新版 I2C Master API。`aw9523b_init()` 会通过该
接口添加地址为 `0x59` 的设备，并读取 `0x10` 寄存器确认芯片 ID 为 `0x23`。
初始化不会执行软件复位；它会将 16 路端口全部切换到 GPIO 模式，按
BOX3 硬件设计把 K1、K2 配置为输入，其余 14 路配置为输出。方向寄存器中
`1=输入、0=输出`，因此 P0/P1 的方向值分别为 `0x03` 和 `0x00`，合并后的
16 位方向掩码是 `0x0003`。初始化不会改写输出锁存寄存器 `0x02/0x03`。

引脚和实验行为参考[正点原子 BOX3 IIC_EXIO 实验](https://wiki.alientek.com/docs/Boards/IoT/DNESP32S3B3/example-idf/iic_exio/)。

原理图中的 16 路端口映射如下：

| AW9523B | BOX3 功能 | 类型/注意事项 |
| --- | --- | --- |
| P0_0 | K1 | 输入，默认高电平，按下为低电平 |
| P0_1 | K2 | 输入，默认低电平，按下为高电平 |
| P0_2 | BAT_CHRG_EN | 输出，电池充电控制 |
| P0_3 | BAT_CHRG | 输出，电池充电控制 |
| P0_4 | ESP_ADC_SEL | 输出，ADC 信号选择 |
| P0_5 | PA_CTRL | 输出，音频功放控制 |
| P0_6/P0_7 | EXT_GPIO0/EXT_GPIO1 | 输出，扩展 GPIO |
| P1_0 | LCD_BL | 输出，LCD 背光控制 |
| P1_1 | LED_RED | 输出，红灯，低电平点亮 |
| P1_2 | LED_BLUE | 输出，蓝灯，低电平点亮 |
| P1_3 | VDD_3V3_EN | 输出，3.3 V 电源控制 |
| P1_4 | VBAT_EN | 输出，电池电源控制 |
| P1_5 | VDDA_3V3_EN | 输出，模拟 3.3 V 电源控制 |
| P1_6 | VDD_2V8_EN | 输出，2.8 V 电源控制 |
| P1_7 | TP_CAM_RESET | 输出，触摸/摄像头复位 |

驱动提供以下接口：

- `aw9523b_read_register()` / `aw9523b_write_register()`：单寄存器访问。
- `aw9523b_configure_gpio()`：指定 P0/P1，并设置端口内引脚的输入或输出方向。
- `aw9523b_read_gpio()` / `aw9523b_write_gpio()`：指定 P0/P1，读取或写入端口内引脚。
- `aw9523b_read_all_inputs()`：一次读取 16 路当前电平。
- `aw9523b_set_box3_led()`：按低电平有效规则控制板载红灯或蓝灯。
- `aw9523b_interrupt_init()`：固定配置 K1/K2 的内部中断掩码。
- `aw9523b_interrupt_process()`：读取并处理 AW9523B 中断状态。
- `aw9523b_soft_reset()`：显式软件复位；会恢复全部端口配置，需谨慎调用。

板载红灯 P1_1、蓝灯 P1_2 均为低电平点亮。P1_3、P1_4 等扩展端口连接板上
电源控制信号，不应在不了解原理图的情况下改写。

AW9523B 的 INTN 与外接触摸屏的 INT 都是低电平有效信号，共用 ESP32-S3
GPIO42。`interrupt_manager.c/.h` 独占该 GPIO 的 ISR；ISR 只通知 FreeRTOS
任务，不访问 I2C。管理任务启用 `0x0001 | 0x0002`，同一次中断依次调用
`aw9523b_interrupt_process()` 和 `tp_interrupt_process()`，读取两颗芯片后分别
处理。若任一设备仍将共享线拉低，管理任务会延时重试，防止因没有新的下降沿
而丢失中断。

本示例固定使能 P0 上 K1 和 K2 的 AW9523B 中断，P0 其余引脚及整个 P1 保持屏蔽。
中断处理读取 P0 输入寄存器 `0x00` 释放 INTN，并按 20 ms 时间窗过滤按键抖动。

### LCD 驱动

`spi.c/.h` 负责 SPI2 总线初始化、板级引脚和 DMA 内存管理；`lcd.c/.h` 使用
ESP-IDF 的 `esp_lcd` ST7789 驱动并提供基础绘图接口；`display.c/.h` 负责 ASCII
字模、字符串排版和测试色条。屏幕以 60 MHz、SPI 模式 0 工作，逻辑分辨率
使用 X=240、Y=320 原生地址，物理原点位于左下角，横轴为 Y。LCD 的 RESX 通过
`ESP_LCD_RESET` 网络与 ESP32-S3 的
`CHIP_PU` 共用，不能作为独立 GPIO 控制，因此面板驱动使用 ST7789 软件复位；背光
通过 AW9523B P1_0 控制，并按低电平有效逻辑封装为 `lcd_backlight_set()`。

驱动使用 20 行 DMA 暂存区分块刷新，并等待每次异步 SPI 传输完成后才复用缓冲区。
填充区域必须完全位于屏幕范围内，越界或空区域返回 `ESP_ERR_INVALID_ARG`。

可用接口：

- `lcd_init()`：初始化 SPI2、ST7789V2 和背光，重复调用安全。
- `lcd_backlight_set()`：打开或关闭背光。
- `lcd_clear()`：使用一个 RGB565 颜色清除整个屏幕。
- `lcd_fill_rect()`：填充指定矩形区域。
- `lcd_show_char()`：显示一个可打印 ASCII 字符。
- `lcd_show_string()`：在指定矩形区域内显示 ASCII 字符串。
- `lcd_show_test_pattern()`：显示八色竖向测试条。

### 触摸驱动

`tp.c/.h` 驱动外接 2.4 英寸电容触摸屏上的 CHSC5432。芯片使用 I2C0、7 位
地址 `0x2E`；复位并非 ESP32-S3 独立 GPIO，而是原理图中的
`TP_CAM_RESET`，由 AW9523B P1_7 控制。该复位信号也与摄像头接口共用。

`tp_init()` 将复位信号拉低再拉高，读取 Boot 版本，以及配置区中的 IC 型号、
配置版本、Project ID、Vendor ID、TP 原始 X/Y 分辨率和最大触点数。全部字段
读取并输出日志后，再确认芯片为 CHSC5432、原始分辨率为 240×320 且支持
5 个触点，从而与 LCD 的 X=240、Y=320 原生坐标范围对应。`tp_read()` 从事件地址
`0x2000002C` 一次读取官方建议的 28 字节，解析最多 5 个触点，并根据 TP 实际
分辨率完成越界检查；输出坐标不再旋转或镜像。`tp_interrupt_process()` 由 GPIO42 共享中断任务
调用，读取事件后通过日志输出触点坐标；应用也可以直接调用 `tp_read()` 获取
`tp_state_t`。

## 工程结构

```text
xc_esp32s3/
|-- CMakeLists.txt
|-- sdkconfig.defaults
|-- main/
|   |-- CMakeLists.txt
|   |-- main.c
|   |-- inc/
|   |   |-- aw9523b.h
|   |   |-- i2c.h
|   |   |-- interrupt_manager.h
|   |   |-- key_interrupt.h
|   |   |-- lcd.h
|   |   |-- spi.h
|   |   `-- tp.h
|   `-- src/
|       |-- aw9523b.c
|       |-- i2c.c
|       |-- interrupt_manager.c
|       |-- key_interrupt.c
|       |-- lcd.c
|       |-- spi.c
|       `-- tp.c
|-- README.md
`-- pdf/
    |-- ATK_DNESP32S3B3_V1.0.pdf
    |-- AW9523BTQR.pdf
    |-- CHSC5xxx.pdf
    `-- ST7789.pdf
```

`sdkconfig.defaults` 已设置：

- ESP32-S3 目标芯片。
- 16 MiB QIO Flash、80 MHz。
- 8 MiB Octal PSRAM、80 MHz。
- USB Serial/JTAG 主控制台。

## `sdkconfig` 与 `sdkconfig.defaults`

这两个文件中出现相同的配置项是正常现象，它们属于配置生成过程中的不同层级：

```text
sdkconfig.defaults
        +
ESP-IDF 内置 Kconfig 默认值和芯片约束
        |
        v
     sdkconfig
        |
        v
      编译固件
```

| 文件 | 作用 | 维护方式 |
| --- | --- | --- |
| `sdkconfig.defaults` | 保存工程所需的关键默认配置 | 手动维护并纳入版本管理 |
| `sdkconfig` | ESP-IDF 生成的完整配置，构建时实际使用 | 由工具生成，不在两个文件中重复手动修改 |
| `sdkconfig.old` | 配置切换时保存的旧配置 | 自动生成，可按需删除 |

### `sdkconfig.old` 的作用

ESP-IDF 在 `menuconfig`、`set-target` 或重新生成配置时，可能先把原来的
`sdkconfig` 备份为 `sdkconfig.old`。它只保存上一次的完整配置，方便对比或恢复，
不会参与当前固件编译；实际编译读取的是 `sdkconfig`。

- 不需要旧配置时，可以直接删除，之后配置再次变化时可能重新生成。
- 需要查看配置变化时，可比较 `sdkconfig.old` 和 `sdkconfig`。
- 需要恢复时，确认它仍适用于当前 ESP-IDF 版本和目标芯片，再执行：

```powershell
Copy-Item sdkconfig.old sdkconfig -Force
idf.py reconfigure
```

`sdkconfig.old` 是本机生成的临时备份，本工程已在 `.gitignore` 中忽略它，
不需要提交到版本库。

例如，`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 先写在 `sdkconfig.defaults` 中；
ESP-IDF 生成当前配置时，会将其写入 `sdkconfig`。这不是重复定义，也不会产生冲突。

`idf.py menuconfig` 修改的是当前 `sdkconfig`，不会自动更新
`sdkconfig.defaults`。如果需要根据当前配置生成一份精简的默认配置，可以执行：

```powershell
idf.py save-defconfig
```

本工程建议只手动维护 `sdkconfig.defaults`。修改它以后，按下面的方式重新生成
`sdkconfig`：

```powershell
idf.py fullclean
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py set-target esp32s3
idf.py build
```

## 开发环境

- ESP-IDF 5.5.x，本工程已使用 ESP-IDF 5.5.5 验证。
- VS Code ESP-IDF 扩展，或已正确激活的 ESP-IDF 命令行环境。
- 支持数据传输的 USB Type-C 线。

建议从 VS Code 命令面板打开 `ESP-IDF: Open ESP-IDF Terminal`，再执行下面的命令。

## 编译

首次编译或切换过芯片型号后执行：

```powershell
cd D:\xc_esp32s3
idf.py set-target esp32s3
idf.py build
```

后续修改代码后只需执行：

```powershell
idf.py build
```

编译成功后，应用固件位于：

```text
build/xc_esp32s3_tool.bin
```

## 烧录与监视

当前电脑将 BOX3 识别为 `COM4`。只烧录固件时执行：

```powershell
idf.py -p COM4 flash
```

烧录完成后立即打开串口监视器时执行：

```powershell
idf.py -p COM4 flash monitor
```

`COM4` 的 USB 设备标识为 `VID_303A&PID_1001`（Espressif）。
按 `Ctrl+]` 退出串口监视器。

烧录、复位或更换 USB 接口后，Windows 可能重新分配端口号。如果 `COM4`
不可用，请在设备管理器中查找 Espressif USB 串行设备，并将命令中的端口号替换为实际值。

## 预期输出

```text
Hello World from ALIENTEK DNESP32S3 BOX3!
ESP-IDF: v5.5.5
Chip: esp32s3, 2 core(s), revision <版本号>
Flash: 16 MiB
PSRAM: 8 MiB total, <可用容量> KiB free
I (...) BOX3: 1 MiB PSRAM read/write test: PASS
I (...) KEY: K0 interrupt ready on GPIO0
I (...) I2C: I2C0 ready: SCL=GPIO2, SDA=GPIO3
I (...) AW9523B: Ready: address=0x59, ID=0x23
I (...) AW9523B: INTN ready, P0 mask=0x03, P1 mask=0x00
I (...) INT_MGR: Shared interrupt ready on GPIO42, flags=0x0001
I (...) SPI: SPI2 ready: SCLK=15, MOSI=16, MISO=17, max transfer=9600 bytes
I (...) LCD: ST7789V2 ready: X=240, Y=320, SPI2 60 MHz, CS=47, DC=48
I (...) KEY: K0 pressed
I (...) BOX3: K1 pressed, red LED on
I (...) BOX3: K2 pressed, blue LED on
I (...) BOX3: Hello World - uptime: 5 s
```

检测结果不是 16 MiB Flash 或 8 MiB PSRAM 时，程序会输出警告日志。
K0 同时是 GPIO0 启动模式按键。程序运行后可以正常触发按键中断；复位或上电时
不要一直按住 K0，否则芯片会进入下载模式。

## 常用命令

```powershell
idf.py menuconfig       # 打开图形配置界面
idf.py build            # 编译工程
idf.py clean            # 清除普通编译产物
idf.py fullclean        # 完全清除构建目录
idf.py flash            # 烧录固件
idf.py monitor          # 查看运行日志
idf.py flash monitor    # 烧录并查看日志
```

## 常见问题

### 找不到下载端口

1. 确认 Type-C 线支持数据传输，而不只是充电。
2. 更换 USB 接口后重新查看串口列表。
3. 必要时按住 K0（GPIO0），按一下复位键，再松开 K0，使芯片进入下载模式。

### 修改了 `sdkconfig.defaults` 但配置没有变化

已有的 `sdkconfig` 仍然保存着当前生效值。请按照前面的
“`sdkconfig` 与 `sdkconfig.defaults`”章节删除旧配置并重新生成。

### 看不到 Hello World 日志

本工程使用 USB Serial/JTAG 控制台。烧录或复位后端口可能会重新枚举，重新执行
`idf.py monitor`，必要时通过 `idf.py -p COM4 monitor` 指定当前端口。
