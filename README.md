# STM32F429_OV5640_FreeRTOS

## 项目简介

本项目基于 **正点原子阿波罗 V2 STM32F429IGT6 开发板**、**正点原子 OV5640 摄像头模块** 和 **正点原子 3.5 寸 MCU 电阻触摸 TFT LCD**，实现一个嵌入式视觉采集、实时显示、上位机导出与图像质量调试系统。

当前工程已经完成基础摄像头采集与显示链路：

```
OV5640 -> DCMI -> DMA -> LCD GRAM -> 3.5 寸 MCU LCD 实时显示
```

同时已经完成 PC Dump 图像导出链路：

```
OV5640 -> DCMI -> DMA -> SRAM Buffer -> USART1 -> Python/OpenCV
```

项目当前重点已经从显示链路调试，进入 **OV5640 图像质量调试阶段**，优先方向为：

```
曝光 / AEC 参数调试
```

本项目不是通用 OV5640 接线项目，而是基于正点原子阿波罗 V2 开发板的固定硬件资源项目。后续所有功能开发都必须遵守该硬件平台的实际连接关系和引脚冲突约束。

------

## 当前项目状态

当前工作分支：

```
feature/pc-frame-dump
```

当前默认实时显示模式：

```
#define CAMERA_MODE CAMERA_MODE_480X320_REAL
```

当前 PC Dump 模式：

```
#define CAMERA_MODE CAMERA_MODE_PC_DUMP_RGB565
```

当前已验证功能：

```
1. OV5640 ID 读取成功，ID = 0x5640
2. LCD 初始化成功，LCD ID = 0x5310
3. OV5640 320x240 RGB565 显示成功
4. OV5640 480x320 RGB565 全屏显示成功
5. DCMI + DMA 直接写 LCD GRAM 实时显示成功
6. PC Dump 图像导出功能完成
7. Python/OpenCV 图像恢复与质量分析工具完成
8. 当前进入 OV5640 曝光 / AEC 图像质量调试阶段
```

------

## 硬件平台

| 模块       | 型号 / 说明                            |
| ---------- | -------------------------------------- |
| MCU        | STM32F429IGT6                          |
| 开发板     | 正点原子阿波罗 V2 STM32F429IGT6 开发板 |
| 摄像头     | 正点原子 OV5640 摄像头模块             |
| 摄像头时钟 | OV5640 模块自带 24 MHz 晶振            |
| 显示屏     | 正点原子 3.5 寸 MCU 电阻触摸 TFT LCD   |
| LCD 控制器 | NT35310                                |
| LCD ID     | 0x5310                                 |
| IO 扩展    | PCF8574                                |
| 调试串口   | USART1，115200，8N1                    |

------

## 硬件连接

### OV5640 SCCB

OV5640 控制总线为 SCCB，当前工程使用 GPIO 软件模拟，不使用 I2C1 扫描器作为主线。

| OV5640 信号 | STM32F429 引脚 | 说明               |
| ----------- | -------------- | ------------------ |
| SIOC        | PB4            | 软件模拟 SCCB 时钟 |
| SIOD        | PB3            | 软件模拟 SCCB 数据 |

注意：

```
PB3 / PB4 与 JTAG 相关功能存在复用关系，调试方式应使用 SWD，避免 Full JTAG 占用这些引脚。
```

### OV5640 控制信号

| 信号     | 引脚 / 控制方式 | 说明                                    |
| -------- | --------------- | --------------------------------------- |
| OV_PWDN  | PCF8574_P2      | 通过 PCF8574 控制，不是 STM32 普通 GPIO |
| OV_RESET | PA15            | 摄像头复位                              |

注意：

```
PA15 也与 JTAG 相关功能存在复用关系，工程中应使用 SWD 调试方式。
```

### OV5640 DVP / DCMI

| OV5640 信号 | STM32F429 引脚 |
| ----------- | -------------- |
| D0          | PC6            |
| D1          | PC7            |
| D2          | PC8            |
| D3          | PC9            |
| D4          | PC11           |
| D5          | PD3            |
| D6          | PB8            |
| D7          | PB9            |
| VSYNC       | PB7            |
| HREF        | PH8            |
| PCLK        | PA6            |

注意：

```
OV5640 模块自带 24 MHz 晶振，当前没有使用 STM32 输出 XCLK。
```

### SDIO 引脚冲突说明

SD 卡使用 SDIO，但 SDIO 与 DCMI 存在引脚复用冲突。

| SDIO 信号 | STM32F429 引脚 | 冲突说明                    |
| --------- | -------------- | --------------------------- |
| SDIO_D0   | PC8            | 与 OV5640 D2 / DCMI_D2 冲突 |
| SDIO_D1   | PC9            | 与 OV5640 D3 / DCMI_D3 冲突 |
| SDIO_D2   | PC10           | 无 DCMI 数据线冲突          |
| SDIO_D3   | PC11           | 与 OV5640 D4 / DCMI_D4 冲突 |
| SDIO_CMD  | PD2            | SDIO 命令线                 |
| SDIO_CK   | PC12           | SDIO 时钟线                 |

因此，当前项目不设计为：

```
摄像头连续采集 + SDIO 同时高速写卡
```

后续 SD 卡功能采用硬件受限下的拍照式保存方案：

```
停止采集 -> 保存单帧图像 -> 写入 SD 卡 -> 恢复采集
```

------

## 当前主要功能链路

### 1. LCD 实时显示链路

```
OV5640
  ↓
DCMI
  ↓
DMA
  ↓
LCD GRAM
  ↓
NT35310 LCD 480x320 显示
```

当前已经完成：

```
1. 320x240 RGB565 显示
2. 480x320 RGB565 全屏显示
3. DCMI + DMA 直接写 LCD GRAM
```

### 2. PC Dump 图像导出链路

```
OV5640
  ↓
DCMI
  ↓
DMA
  ↓
SRAM Buffer
  ↓
USART1
  ↓
Python/OpenCV
```

PC Dump 当前模式：

```
#define CAMERA_MODE CAMERA_MODE_PC_DUMP_RGB565
```

TestBar / RealImage 选择：

```
#define PC_DUMP_USE_REAL_IMAGE 0U   // TestBar
#define PC_DUMP_USE_REAL_IMAGE 1U   // RealImage
```

PC Dump 命令流程：

```
1. Python 打开串口
2. Python 发送 DUMP\n
3. STM32 收到命令
4. STM32 采集 160x120 RGB565 snapshot
5. STM32 发送 OV56RGB5 + header + payload + CRC
6. Python 接收数据并校验 CRC
7. Python 保存图像、灰度图、直方图和质量分析报告
```

运行命令：

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200
```

带标签运行：

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_baseline
```

------

## PC Dump 输出文件

Python 工具会自动创建 `captures/` 目录，并生成以下文件：

```
ov5640_dump.png
ov5640_gray.png
ov5640_hist.png
ov5640_report.txt

captures/001_real_baseline_时间.png
captures/001_real_baseline_时间_gray.png
captures/001_real_baseline_时间_hist.png
captures/001_real_baseline_时间_report.txt
captures/summary.csv
```

主要分析指标：

```
mean_brightness
shadow_ratio
highlight_ratio
R mean
G mean
B mean
R/G ratio
B/G ratio
B/R ratio
Laplacian variance
```

------

## 当前 baseline 图像质量结果

当前 RealImage baseline 测试结果：

```
Resolution: 160x120
Payload length: 38400 bytes
Mean brightness: 106.251
Min brightness: 8
Max brightness: 255
Shadow ratio: 0.229%
Highlight ratio: 8.604%
R mean: 105.354
G mean: 104.549
B mean: 117.279
R/G ratio: 1.008
B/G ratio: 1.122
B/R ratio: 1.113
R/B ratio: 0.898
Laplacian variance: 623.007
Blur threshold: 100.0
```

分析结论：

```
1. 数据长度正确，160 x 120 x 2 = 38400 bytes
2. CRC 通过，串口传输正常
3. 平均亮度 106，整体亮度正常
4. Shadow ratio 很低，没有明显欠曝
5. Highlight ratio = 8.604%，存在一定过曝
6. B/R ratio = 1.113，略微偏蓝但仍在可接受范围
7. Laplacian variance = 623，清晰度正常
```

当前主要画质问题：

```
高光比例偏高，存在一定过曝。
```

------

## 480x320 LCD 显示调试结论

一开始 480x320 出现雪花，经过分辨率测试发现：

```
320x240 正常
320x320 正常
400x240 雪花
480x240 雪花
480x320 雪花
```

说明问题与：

```
每行宽度 / LCD GRAM 写入速度
```

有关。

最后对比正点原子参考工程，确认关键问题是 FMC/LCD 写时序太慢。

最终采用：

```
#define LCD_MCU_FAST_WRITE_TIMING_ENABLE  1
#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6
```

测试结果：

```
15/15 雪花
10/10 雪花
8/8 正常
6/6 正常，最终采用
4/4 正常
3/3 正常
```

------

## 主要源码文件

### OV5640 / SCCB

```
BSPDrivers/Inc/OV5640.h
BSPDrivers/Src/OV5640.c
BSPDrivers/Inc/ov5640cfg.h
bsp_sccb.h
bsp_sccb.c
```

### DCMI / DMA

```
BSPDrivers/Inc/camera_dcmi_dma.h
BSPDrivers/Src/camera_dcmi_dma.c
```

### PC Dump

```
BSPDrivers/Inc/camera_pc_dump.h
BSPDrivers/Src/camera_pc_dump.c
tools/pc_dump_rgb565.py
PC_DUMP_LOG.md
```

### LCD

```
LCD_MCU driver for NT35310
```

### 后续图像调参文件

后续计划新增：

```
BSPDrivers/Inc/ov5640_tuning.h
BSPDrivers/Src/ov5640_tuning.c
AEC_TUNING_LOG.md
```

------

## 项目目录结构

```
ISP_OV5640/
├── BSPDrivers/
│   ├── Inc/
│   └── Src/
├── Core/
│   ├── Inc/
│   └── Src/
├── Drivers/
├── tools/
│   └── pc_dump_rgb565.py
├── docs/
│   └── archive/
│       └── TUNING_PLAN_480x320_DONE.md
├── PC_DUMP_LOG.md
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── CMakePresets.json
├── ISP_OV5640.ioc
├── startup_stm32f429xx.s
├── STM32F429XX_FLASH.ld
└── .gitignore
```

------

## 当前开发计划

本项目后续计划以 `embedded_vision_roadmap_apollo_v2_corrected.docx` 中的硬件修正版路线图为准。

该项目不是通用 OV5640 接线项目，而是基于正点原子阿波罗 V2 STM32F429IGT6 开发板的固定硬件资源项目。后续所有功能都必须遵守以下硬件约束：

```
1. OV5640 SCCB 使用 PB3 / PB4 软件模拟，不使用 I2C1 扫描器作为主线。
2. OV_PWDN 由 PCF8574_P2 控制，不是 STM32 普通 GPIO。
3. OV_RESET 使用 PA15。
4. OV5640 模块自带 24 MHz 晶振，当前不使用 STM32 输出 XCLK。
5. DCMI 数据线固定为 PC6 / PC7 / PC8 / PC9 / PC11 / PD3 / PB8 / PB9。
6. SDIO 与 DCMI 在 PC8 / PC9 / PC11 上存在复用冲突。
7. SD 卡保存不能设计成“DCMI 连续采集 + SDIO 同时高速写卡”，应采用拍照式保存或上位机导出方案。
```

------

### 阶段 1：板级基础与硬件约束整理

状态：已完成 / 持续完善文档。

目标：

```
1. 建立 STM32F429 工程模板。
2. 完成串口日志输出。
3. 完成 LED 心跳或基础运行状态指示。
4. 阅读 OV5640、LCD、SDIO 相关原理图。
5. 记录 PB3/PB4、PA15、PCF8574_P2、DCMI/SDIO 冲突等硬件约束。
```

对应输出：

```
1. 串口日志可用。
2. 基础工程可编译下载。
3. README.md / AGENTS.md / PC_DUMP_LOG.md 等文档记录硬件约束。
```

------

### 阶段 2：OV5640 控制链路

状态：已完成。

目标：

```
1. 实现 PB3 / PB4 软件 SCCB。
2. 支持 OV5640 16 位寄存器地址 + 8 位数据读写。
3. 实现 PCF8574 驱动。
4. 通过 PCF8574_P2 控制 OV_PWDN。
5. 使用 PA15 控制 OV_RESET。
6. 完成 OV5640 上电、PWDN、RESET 时序。
7. 读取 OV5640 ID。
```

完成标准：

```
1. SCCB 读写正常。
2. OV5640 ID = 0x5640。
3. 复位后 SCCB 可稳定访问。
4. 代码不依赖 I2C1 扫描 OV5640。
```

------

### 阶段 3：OV5640 输出格式与测试图

状态：已完成。

目标：

```
1. 整理 OV5640 初始化表。
2. 配置 OV5640 RGB565 输出。
3. 支持测试彩条输出。
4. 支持真实图像输出。
5. 支持不同输出尺寸配置。
```

完成标准：

```
1. LCD 可显示 OV5640 测试彩条。
2. LCD 可显示 OV5640 真实图像。
3. 后续调试可以在 TestBar / RealImage 之间切换。
```

------

### 阶段 4：DCMI + DMA 图像采集

状态：已完成。

目标：

```
1. 按阿波罗 V2 固定引脚配置 DCMI。
2. 配置 DMA 接收 OV5640 并口数据。
3. 实现帧中断或帧完成回调。
4. 支持 DCMI + DMA 图像接收。
```

完成标准：

```
1. DCMI 可接收到帧。
2. DMA 可搬运图像数据。
3. 串口可打印帧接收事件。
4. 不在该阶段同时启用 SDIO 写卡。
```

------

### 阶段 5：LCD 实时显示

状态：已完成。

目标：

```
1. 实现 OV5640 -> DCMI -> DMA -> LCD GRAM 实时显示。
2. 支持 320x240 RGB565 显示。
3. 支持 480x320 RGB565 全屏显示。
4. 解决 480x320 雪花问题。
```

完成标准：

```
1. 320x240 实时显示正常。
2. 480x320 实时显示正常。
3. DCMI + DMA 直接写 LCD GRAM 路径稳定。
4. LCD/FMC 写时序已优化。
```

最终采用的 LCD 写时序：

```
#define LCD_MCU_FAST_WRITE_TIMING_ENABLE  1
#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6
```

------

### 阶段 6：PC Dump 上位机导出

状态：已完成。

目标：

```
1. 实现 OV5640 -> DCMI -> DMA -> SRAM Buffer。
2. 实现 USART1 图像数据导出。
3. 支持 Python 命令触发式 DUMP。
4. 添加 header、payload、CRC。
5. Python 端完成 RGB565 图像恢复。
6. Python/OpenCV 输出图像质量分析结果。
```

完成标准：

```
1. Python 发送 DUMP\n 后，STM32 采集一帧 snapshot。
2. 串口传输 payload 长度正确。
3. CRC 校验通过。
4. Python 可保存 PNG、灰度图、直方图、report。
5. summary.csv 可持续记录测试结果。
```

------

### 阶段 7：OV5640 图像质量调试

状态：当前进行中。

优先级：

```
1. 曝光 / AEC
2. 增益限制
3. AWB 白平衡
4. 亮度
5. 对比度
6. 饱和度
7. 锐度
8. 镜像 / 翻转
```

当前第一目标：

```
Highlight ratio: 8.604% -> 3%~5%
Mean brightness: 保持 90~130
B/R ratio: 保持 0.9~1.15
Laplacian variance: 不明显下降
```

完成标准：

```
1. 新增 ov5640_tuning.c/.h。
2. 支持寄存器 dump。
3. 支持 AEC 参数小步调试。
4. 每次调参都通过 PC Dump 量化分析。
5. 每轮测试结果写入 AEC_TUNING_LOG.md。
```

------

### 阶段 8：串口 CLI 在线调参

状态：待实现。

目标：

```
1. 实现串口命令解析。
2. 支持在线读写 OV5640 寄存器。
3. 支持在线切换曝光、白平衡、亮度、对比度、饱和度、锐度等参数。
4. 支持调参后立即 PC Dump 保存结果。
```

计划命令示例：

```
cam rd 4741
cam wr 4741 00
cam dump
cam ev -1
cam awb auto
cam bright 0
cam contrast 1
cam sat 2
cam sharp 1
```

完成标准：

```
1. 不重新编译即可在线修改关键寄存器。
2. 每次调参都有 tag 和 summary.csv 记录。
3. CLI 不影响默认 480x320 实时显示模式。
```

------

### 阶段 9：FreeRTOS 多任务化

状态：待实现。

目标：

```
1. 将当前裸机流程逐步迁移到 FreeRTOS。
2. 将相机控制、显示刷新、串口命令、状态日志等功能拆分为任务。
3. 加入队列、事件标志或信号量管理帧完成事件。
4. 保证已有裸机功能不被破坏。
```

计划任务：

| 任务名称    | 功能                              |
| ----------- | --------------------------------- |
| CameraTask  | OV5640 初始化、参数配置、采集控制 |
| DisplayTask | LCD 显示刷新或显示模式管理        |
| CliTask     | 串口命令解析与在线调参            |
| DebugTask   | 日志输出、状态监控、帧率统计      |
| StorageTask | 后续拍照式 SD 卡保存扩展          |

完成标准：

```
1. FreeRTOS 下 480x320 实时显示正常。
2. CLI 调参功能正常。
3. PC Dump 功能可用。
4. 任务间同步关系清晰。
```

------

### 阶段 10：帧率统计与稳定性测试

状态：待实现。

目标：

```
1. 统计 DCMI 帧率。
2. 统计 PC Dump 传输耗时。
3. 统计 LCD 实时显示稳定性。
4. 长时间运行测试。
5. 记录异常帧、CRC 错误、DMA 错误和 DCMI 错误。
```

完成标准：

```
1. 可输出实时 FPS。
2. 可记录连续运行测试结果。
3. 可证明系统稳定运行。
```

------

### 阶段 11：SD 卡拍照式保存

状态：待实现，硬件受限扩展项。

由于 SDIO 的 PC8 / PC9 / PC11 与 DCMI 的数据线复用冲突，本项目不把 SD 卡设计为“实时视频流边采边写”。

本阶段目标是实现：

```
停止采集 -> 保存单帧图像 -> 写入 SD 卡 -> 恢复采集
```

目标：

```
1. 配置 SDIO / FATFS。
2. 实现单帧图像保存。
3. 支持 RAW 或 BMP 文件格式。
4. 写卡前停止 DCMI。
5. 必要时重配冲突引脚为 SDIO。
6. 写卡完成后恢复 DCMI 采集。
```

完成标准：

```
1. 能保存单帧图像文件到 SD 卡。
2. 保存流程不破坏后续摄像头采集。
3. README / 日志中明确说明 DCMI 与 SDIO 的引脚冲突和工程取舍。
```

------

### 阶段 12：项目总结与简历化

状态：待实现。

目标：

```
1. 整理项目架构图。
2. 整理关键调试日志。
3. 整理硬件冲突分析。
4. 整理图像质量调参结果。
5. 整理 FreeRTOS 任务划分。
6. 整理 PC Dump / OpenCV 分析工具说明。
7. 形成可用于简历和面试讲解的项目描述。
```

最终项目能力表达：

```
1. 根据原理图完成固定硬件平台下的摄像头驱动适配。
2. 实现 PB3/PB4 软件 SCCB、PCF8574_P2 PWDN、PA15 RESET。
3. 实现 OV5640 ID 读取、寄存器配置、RGB565 输出。
4. 基于 DCMI + DMA 完成 LCD 实时显示。
5. 通过 PC Dump + Python/OpenCV 建立图像质量量化分析流程。
6. 针对 SDIO 与 DCMI 引脚冲突设计拍照式保存或上位机导出方案。
7. 从曝光、增益、噪声、动态范围和链路稳定性角度完成项目调试与优化。
```

------

## 当前开发约束

当前阶段不要修改：

```
1. 不重写 SCCB 驱动
2. 不重写 LCD 驱动
3. 不重写 DCMI/DMA 显示路径
4. 不修改 GPIO 引脚
5. 不修改已稳定的 OV5640 初始化表
6. 不改 SD 卡相关功能
7. 不加入 framebuffer 软件缩放
8. 不做大规模重构
```

当前调参原则：

```
1. 每次只改一个参数
2. 每次修改都能回退
3. 每次测试都要用 PC Dump 记录结果
4. 每次调参结果都写入日志
5. 优先使用量化指标，而不是只凭肉眼判断
```

------

## Git 分支与文档

当前工作分支：

```
feature/pc-frame-dump
```

当前文档：

```
README.md                                  当前项目状态说明
AGENTS.md                                  给 AI / Codex 的工程约束说明
PC_DUMP_LOG.md                             PC Dump 阶段日志
docs/archive/TUNING_PLAN_480x320_DONE.md   480x320 阶段归档计划
```

后续准备新增：

```
AEC_TUNING_LOG.md
```

------

## 项目目标

本项目用于学习和实践 STM32F429 平台下摄像头采集系统的完整开发流程，包括：

```
1. 摄像头上电、复位和寄存器配置
2. SCCB / I2C 通信
3. PCF8574 扩展 IO 控制
4. OV5640 RGB565 输出配置
5. DCMI 图像采集
6. DMA 数据搬运
7. MCU LCD 实时显示
8. 串口图像导出
9. Python/OpenCV 图像质量分析
10. OV5640 曝光、白平衡、颜色和锐度调试
11. 串口 CLI 在线调参
12. FreeRTOS 多任务化
13. 帧率统计与稳定性测试
14. SD 卡拍照式保存
15. 项目总结与简历化表达
```

当前项目可作为嵌入式摄像头驱动、STM32 外设综合应用、图像采集链路调试和工程化开发实践项目。