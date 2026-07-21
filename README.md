# STM32F429_OV5640_FreeRTOS

## 项目简介

这是基于 **正点原子阿波罗 V2 STM32F429IGT6**、**OV5640 摄像头模块**和 **3.5 寸 MCU TFT LCD** 开发的嵌入式图像采集项目。

目前已经完成 OV5640 驱动、DCMI + DMA 图像采集、LCD 实时显示、串口图像导出、图像参数调试、双缓冲、基础图像处理和 UART CLI 在线调参。

下一阶段准备将现有裸机程序迁移到 FreeRTOS，并继续完成帧率统计、稳定性测试和 SD 卡单帧保存。

## 系统链路

```text
OV5640
  ↓
DCMI + DMA
  ↓
图像缓冲区
  ├── LCD 实时显示
  ├── 灰度化 / 二值化
  └── USART1 -> Python 图像导出
```

## 硬件平台

| 模块 | 型号 |
|---|---|
| MCU | STM32F429IGT6 |
| 开发板 | 正点原子阿波罗 V2 |
| 摄像头 | 正点原子 OV5640 |
| 显示屏 | 正点原子 3.5 寸 MCU TFT LCD |
| LCD 控制器 | NT35310 |
| IO 扩展 | PCF8574 |
| 调试器 | ST-Link |
| 调试串口 | USART1，115200，8N1 |

## 硬件连接

### OV5640 控制接口

| 信号 | 连接方式 |
|---|---|
| SIOC | PB4，GPIO 软件 SCCB |
| SIOD | PB3，GPIO 软件 SCCB |
| RESET | PA15 |
| PWDN | PCF8574_P2 |
| XCLK | OV5640 模块板载 24 MHz 晶振 |

OV5640 使用 PB3、PB4 软件模拟 SCCB，不使用 STM32 硬件 I2C1。

### OV5640 DCMI 接口

| OV5640 信号 | STM32F429 引脚 |
|---|---|
| D0 | PC6 |
| D1 | PC7 |
| D2 | PC8 |
| D3 | PC9 |
| D4 | PC11 |
| D5 | PD3 |
| D6 | PB8 |
| D7 | PB9 |
| VSYNC | PB7 |
| HREF | PH8 |
| PCLK | PA6 |

PB3、PB4 和 PA15 与 JTAG 功能存在复用关系，因此项目使用 SWD 调试。

## 已完成功能

### OV5640 驱动

- 完成 OV5640 上电、复位和 PWDN 控制
- 完成 PB3、PB4 软件 SCCB
- 支持 16 位寄存器地址和 8 位寄存器数据读写
- 成功读取 OV5640 ID：`0x5640`
- 支持测试彩条和真实图像输出
- 支持 RGB565 输出格式

### DCMI + DMA 图像采集

- 完成 DCMI 固定引脚配置
- 完成 DMA 图像数据搬运
- 支持 OV5640 图像帧采集
- 支持采集到 LCD GRAM
- 支持采集到 SRAM 图像缓冲区

### LCD 实时显示

- 支持 320×240 RGB565 显示
- 支持 480×320 RGB565 全屏显示
- 解决 480×320 显示雪花问题
- DCMI + DMA 直接写 LCD GRAM 链路运行正常

当前稳定的 FMC/LCD 写时序：

```c
#define LCD_MCU_WRITE_ADDRESS_SETUP  6
#define LCD_MCU_WRITE_DATA_SETUP     6
```

### PC Dump 图像导出

实现了命令触发式单帧图像导出：

```text
Python 发送 DUMP
        ↓
STM32 采集 160×120 RGB565 图像
        ↓
USART1 发送 OV56RGB5 数据包
        ↓
Python 校验 CRC 并恢复图像
```

数据包格式：

```text
OV56RGB5 + Header + RGB565 Payload + CRC
```

Python 工具：

```text
tools/pc_dump_rgb565.py
```

运行示例：

```bash
python tools/pc_dump_rgb565.py --port COM6 --baud 115200 --tag test
```

Python 工具可以生成：

- RGB 图像
- 灰度图
- 灰度直方图
- 图像分析报告
- CSV 测试记录

### OV5640 图像参数调试

已经完成以下参数的对比测试：

- AEC 曝光目标
- AWB 白平衡模式
- 亮度
- 对比度
- 饱和度
- 锐度

当前保留配置：

```text
AEC        = Baseline
AWB        = Auto
Brightness = +1
Contrast   = 0
Saturation = 1
Sharpness  = 0
```

每次测试都通过 PC Dump 导出图像，并使用 Python 统计亮度、高光比例、阴影比例、颜色通道比例和清晰度。

### RGB565 双缓冲

实现了两个 160×120 RGB565 静态缓冲区：

```text
每帧大小：160 × 120 × 2 = 38400 Byte
缓冲数量：2
总占用：76800 Byte
```

工作流程：

```text
DCMI/DMA 写入 Back Buffer
          ↓
采集完成后交换 Front/Back Buffer
          ↓
PC Dump 从 Front Buffer 发送
```

双缓冲可以避免发送图像时，采集数据覆盖当前帧。

### 基础图像处理

当前支持三种模式：

```text
BYPASS      原图输出
GRAYSCALE   灰度图输出
BINARY      二值图输出
```

灰度转换使用整数计算：

```text
Gray = (77 × R + 150 × G + 29 × B) >> 8
```

二值化默认阈值：

```text
Threshold = 128
```

### UART CLI 在线调参

实现了一个简单的 UART CLI，可以在不重新编译程序的情况下切换图像处理模式和二值化阈值。

支持命令：

```text
HELP
STATUS
PROC
PROC BYPASS
PROC GRAY
PROC BINARY
THR
THR 0..255
RESET
DUMP
```

切换灰度模式：

```text
PROC GRAY
STATUS
```

切换二值化模式：

```text
PROC BINARY
THR 128
STATUS
```

恢复默认设置：

```text
RESET
```

CLI 文本响应和 DUMP 二进制数据分开处理，原有 OV56RGB5 协议保持不变。

## 当前进度

- [x] STM32F429 基础工程
- [x] PCF8574 扩展 IO 控制
- [x] 软件 SCCB
- [x] OV5640 ID 读取
- [x] OV5640 RGB565 输出
- [x] DCMI + DMA 图像采集
- [x] 320×240 LCD 显示
- [x] 480×320 LCD 全屏显示
- [x] PC Dump 图像导出
- [x] Python 图像恢复和分析
- [x] AEC / AWB 参数调试
- [x] 亮度、对比度、饱和度和锐度调试
- [x] RGB565 双缓冲
- [x] 灰度化和二值化
- [x] UART CLI 在线调参
- [ ] FreeRTOS 多任务化
- [ ] 帧率统计
- [ ] 长时间稳定性测试
- [ ] SD 卡单帧保存
- [ ] 项目总结和简历整理

## 后续计划

### 阶段 7：FreeRTOS 多任务化

准备将当前裸机程序逐步迁移到 FreeRTOS，计划划分以下任务：

| 任务 | 功能 |
|---|---|
| CameraTask | 摄像头采集控制 |
| ProcessTask | 灰度化和二值化处理 |
| CliTask | UART CLI 命令处理 |
| DumpTask | PC Dump 图像发送 |
| MonitorTask | 帧率和运行状态统计 |

任务之间计划使用队列、信号量或事件标志组同步。

这一阶段首先保证现有的 DUMP、CLI 和图像处理功能不被破坏。

### 阶段 8：帧率和稳定性测试

计划实现：

- 统计摄像头采集帧率
- 统计图像处理耗时
- 统计串口图像传输耗时
- 记录 DMA 和 DCMI 错误
- 记录 CRC 错误
- 进行长时间运行测试

### 阶段 9：SD 卡单帧保存

SDIO 的 PC8、PC9、PC11 与 DCMI 数据线冲突，因此不能直接实现摄像头连续采集和 SDIO 同时写卡。

计划采用拍照式保存：

```text
停止 DCMI
    ↓
保存当前单帧
    ↓
切换冲突引脚
    ↓
写入 SD 卡
    ↓
恢复 DCMI
```

计划支持 RAW 或 BMP 图像保存。

### 阶段 10：项目总结

计划完成：

- 整理系统架构
- 整理 FreeRTOS 任务关系
- 整理图像参数调试结果
- 整理问题定位过程
- 完善 README
- 整理简历项目描述

## 主要文件

```text
BSPDrivers/Inc/
├── OV5640.h
├── camera_dcmi_dma.h
├── camera_pc_dump.h
├── camera_frame_buffer.h
├── camera_image_process.h
├── camera_cli.h
└── ov5640_tuning.h

BSPDrivers/Src/
├── OV5640.c
├── camera_dcmi_dma.c
├── camera_pc_dump.c
├── camera_frame_buffer.c
├── camera_image_process.c
├── camera_cli.c
└── ov5640_tuning.c

Core/Src/
└── main.c

tools/
└── pc_dump_rgb565.py
```

## 项目总结

这个项目主要完成了以下内容：

- 根据原理图确认摄像头连接和引脚冲突
- 使用 GPIO 实现 SCCB 通信
- 配置 OV5640 寄存器和 RGB565 输出
- 使用 DCMI + DMA 接收图像
- 使用 FMC 驱动 MCU LCD
- 管理 RGB565 双缓冲
- 通过串口传输二进制图像
- 使用 Python 恢复和分析图像
- 实现灰度化和二值化
- 实现 UART CLI 在线调参
- 为后续 FreeRTOS 多任务化做准备
