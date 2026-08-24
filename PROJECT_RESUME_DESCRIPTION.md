# 简历项目描述：STM32F429 + OV5640 图像采集与 SD 卡存储系统

## 1. 简历项目名称

1. **基于 STM32F429 + OV5640 的嵌入式图像采集与 SD 卡存储系统（推荐）**
2. 基于 FreeRTOS 的 STM32 图像采集、处理与本地存储系统
3. STM32F429 摄像头图像采集与 SDIO/FatFs 存储项目

推荐第 1 个名称，器件、功能和项目边界最清晰，便于面试官快速识别项目技术栈。

## 2. 简历短版

基于 STM32F429、OV5640、FreeRTOS 和 FatFs 设计嵌入式图像采集与本地存储系统，实现 UART 图像传输、灰度/二值处理、SD 卡 BMP 图片保存和文件递增命名。针对 OV5640 DVP 与 SDIO 引脚复用导致的读写失败问题，通过 SD-only boot 对照实验和寄存器定位，采用 `0x3018[6:4]` 软件隔离方案释放共享线，实现 SDIO takeover、写卡后恢复摄像头链路和 PC 端自动化稳定性测试。

## 3. 简历详细版

**项目名称：** 基于 STM32F429 + OV5640 的嵌入式图像采集与 SD 卡存储系统

**项目环境：** STM32F429、OV5640、FreeRTOS、HAL、UART、DCMI/DVP、DMA、SDIO、FatFs、Python、CMake、Git

**项目描述：** 在资源受限 MCU 上实现从摄像头采集、基础处理、串口传输到 SD 卡本地保存的完整图像链路，并解决 DVP 与 SDIO 外设共享引脚造成的总线冲突。

**主要工作：**

- 完成 OV5640 初始化、DCMI/DMA 图像采集、160×120 RGB565 帧管理和 UART DUMP 图像传输。
- 实现 BYPASS、灰度、二值化处理模式，以及 CLI 在线模式切换和 0～255 阈值调整。
- 基于 FreeRTOS 完成摄像头服务任务、状态监控、IWDG 保护和 heap/stack/fault 运行健康输出。
- 设计 front/back 双缓冲，隔离采集 buffer 与图像处理、UART 发送、SD 保存使用的稳定 front buffer。
- 通过 SD-only boot 对照实验定位 OV5640 DVP 与 SDIO 引脚复用冲突，使用 `0x3018[6:4]` mask 实现安全 SDIO takeover。
- 接入 FatFs，实现 RGB565 到 BMP24 的逐行转换、`IMGxxxx.BMP` 递增保存、错误清理和分阶段耗时统计。
- 编写 Python 自动化工具，校验多轮 SD Snapshot 的文件名连续性、BMP 大小、源数据有效性、写入结果和运行健康状态。

**项目成果：**

- 支持 UART DUMP 和 SD SNAPSHOT 两种图像输出方式。
- 支持 160×120 BMP24 图片保存，文件大小固定为 57654 bytes。
- 支持 `IMG0001.BMP`～`IMG9999.BMP` 8.3 文件名递增保存且不覆盖已有文件。
- SD Snapshot 连续自动化测试正常，具备状态、错误码和 total/prepare/write/cleanup 耗时统计。
- 写卡后摄像头链路可恢复，DUMP 和 binary image request 回归正常。

## 4. 简历精简 bullet 版

- 基于 STM32F429 + OV5640 + FreeRTOS 搭建 DCMI/DMA 图像采集链路，设计 front/back 双缓冲并实现 160×120 RGB565 的处理、UART 传输与运行健康监控。
- 通过 SD-only boot 对照实验定位 OV5640 DVP 与 SDIO 共享引脚冲突，基于 `0x3018[6:4]` mask 释放 D2/D3/D4，完成 SDIO takeover 与摄像头链路恢复。
- 接入 FatFs，实现 RGB565 逐行转换 BMP24、57654-byte 文件写入及 `IMG0001.BMP`～`IMG9999.BMP` 无覆盖递增命名。
- 编写 Python 自动化稳定性测试工具，连续校验 snapshot 结果、文件序列、源图像有效性、各阶段耗时和 RTOS/UART/IWDG 健康状态。

## 5. 项目亮点

1. 不是简单移植 SD 卡例程，而是在摄像头 DVP 与 SDIO 共享引脚条件下完成问题复现、变量隔离、寄存器定位和根因闭环。
2. 使用 SD-only boot 对照实验区分 SDIO 硬件链路问题与外设共享线问题，避免只靠降速和反复改参数试错。
3. 将 DUMP 与 SD SNAPSHOT 统一到 `Camera_RTOS_PrepareRgb565Frame()`，消除两套采集/commit/处理逻辑带来的状态不一致。
4. 为 SD 会话设计完整的 DVP mask、GPIO takeover、FatFs 写入和失败路径 cleanup/restore。
5. 具备 CLI 状态、错误码、耗时统计、PC 图像分析和自动化稳定性测试，形成可验证的工程闭环。

## 6. 面试一句话介绍

### 30 秒版本

我用 STM32F429、OV5640 和 FreeRTOS 做了一个嵌入式图像采集与 SD 卡存储系统，支持 DCMI/DMA 采集、UART DUMP、灰度/二值处理和 BMP24 保存。项目最关键的问题是 OV5640 DVP 与 SDIO 共享引脚，我通过 SD-only boot 对照实验定位到传感器持续驱动共享线，最终用 `0x3018[6:4]` mask 完成总线切换和写卡后的摄像头恢复。

### 1 分钟版本

项目基于 STM32F429 和 OV5640，使用 DCMI/DMA 采集 160×120 RGB565 图像，并通过 FreeRTOS 摄像头任务、front/back 双缓冲和 CLI 支持 UART DUMP、灰度、二值化处理。后续接入 SDIO 和 FatFs 保存 BMP24 时，正常摄像头环境下 SDIO 初始化一直失败，但 SD-only boot 连续读取 block 0 和 block 2048 正常。我据此排除了 SD 卡和 SDIO 硬件，定位到 OV5640 DVP D2/D3/D4 与 PC8/PC9/PC11 复用冲突，通过保存并 mask `0x3018[6:4]` 释放共享线。最终实现 `IMGxxxx.BMP` 递增保存、统一 cleanup/restore、耗时统计和 Python 自动稳定性测试，写卡后 DUMP 仍能正常工作。

### 3 分钟版本

这个项目的目标是在 STM32F429 上完成从摄像头采集到 PC 传输和 SD 卡本地存储的完整链路。前端使用 OV5640 输出 RGB565，MCU 通过 DCMI/DMA 采集；软件上使用 FreeRTOS 摄像头服务任务和 front/back 双缓冲，把采集中的 buffer 与处理、UART 发送、SD 保存使用的 front buffer 隔离。CLI 支持模式切换、二值阈值、状态查询、DUMP 和 SD SNAPSHOT，PC 端工具负责图像恢复、CRC 校验和连续测试。

项目最难的部分是 DVP 和 SDIO 引脚复用。正常工程里 `HAL_SD_Init()` 失败，但关闭摄像头功能的 SD-only boot 可以稳定执行 ATK1B 1-bit polling 读取，这说明 SD 卡、卡座和 SDIO 硬件都正常。进一步映射引脚后发现 OV5640 D2/D3/D4 分别占用 PC8/PC9/PC11，即 SDIO D0/D1/D3。单纯降速不能消除传感器对线路的主动驱动，所以最终保存 `0x3018`，用 `saved_3018 & 0x8F` 临时关闭对应 DVP 输出，SD 会话结束后再恢复原值。

另一个问题是初版 `IMAGE.RGB` 大小正确但全 0。我判断写卡链路已经成功，故障在图像源，于是把 DUMP 已验证的 capture/wait/commit/process 路径抽成公共 `Camera_RTOS_PrepareRgb565Frame()`，让两种功能使用同一帧准备逻辑。最终系统能保存 160×120 BMP24，使用 `IMG0001.BMP` 到 `IMG9999.BMP` 递增命名，并对 total、prepare、write、cleanup 分阶段计时。Python 稳定性脚本会连续执行 snapshot，检查文件序列、57654-byte 文件大小、源数据统计、FatFs 阶段结果和系统健康状态，形成了从定位、实现到回归验证的完整闭环。
