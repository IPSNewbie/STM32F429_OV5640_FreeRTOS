# STM32F429 + OV5640 图像采集与 SD 卡存储项目总结

## 1. 项目概述

本项目基于 STM32F429、OV5640、FreeRTOS、UART、SDIO 和 FatFs，实现嵌入式图像采集、串口图像传输、基础图像处理、SD 卡 BMP 图片保存和 PC 端自动化稳定性测试。项目重点不只是功能实现，还完整经历了图像数据链路搭建、RTOS 化、buffer 生命周期治理、DVP/SDIO 共享线冲突定位以及失败路径恢复验证。

## 2. 硬件平台

1. 主控：STM32F429IGT6。
2. 摄像头：OV5640，DVP 8-bit 输出。
3. 显示屏：3.5 寸 LCD，控制器 NT35310。
4. 图像采集接口：DCMI + DMA。
5. SD 卡接口：SDIO。
6. 通信接口：UART，PC 工具默认 COM4、115200 baud。
7. 关键共享线：
   - OV_D2 → PC8 → SDIO_D0。
   - OV_D3 → PC9 → SDIO_D1。
   - OV_D4 → PC11 → SDIO_D3。

## 3. 软件架构

1. `camera_rtos`
   - 承载 FreeRTOS 摄像头服务任务和运行监控。
   - 负责图像采集、等待、front/back commit 和处理调度。
   - 提供公共接口 `Camera_RTOS_PrepareRgb565Frame()`，供 DUMP 和 SD SNAPSHOT 共用。
2. `camera_frame_buffer`
   - 管理 front/back 双缓冲。
   - 分离正在采集的 buffer 与发送、处理、保存使用的稳定 front buffer。
3. `camera_pc_dump`
   - 通过 UART 输出 160×120 RGB565 图像。
   - 使用 OV56RGB5 帧头、payload 和 CRC32 协议。
4. `camera_image_process`
   - 支持 BYPASS、GRAYSCALE、BINARY 三种模式。
   - 二值化阈值可通过 CLI 在线调整。
5. `camera_cli`
   - 负责串口文本命令解析和简洁状态输出。
6. `camera_sd_storage`
   - 管理 DVP mask、SDIO takeover、FatFs 会话和统一 cleanup/restore。
   - 完成 RGB565 到 BMP24 的逐行转换、文件递增命名、错误码和耗时缓存。
7. `tools`
   - 提供 PC 端图像请求、恢复、质量分析和连续请求工具。
   - 提供 `uart_sd_snapshot_stability.py` 自动执行并校验多轮 SD SNAPSHOT。

## 4. 最终 CLI 命令

```text
HELP
STATUS
PROC [BYPASS|GRAY|BINARY]
THR [0..255]
RESET
DUMP
SD STATUS
SD SNAPSHOT
```

| 命令 | 用途 |
|---|---|
| `HELP` | 显示最终命令列表 |
| `STATUS` | 显示处理模式、运行健康、fault、UART DMA 和 IWDG 状态 |
| `PROC` | 查询或切换 BYPASS、GRAY、BINARY 图像处理模式 |
| `THR` | 查询或设置 0～255 二值化阈值 |
| `RESET` | 恢复 CLI 图像处理参数默认值 |
| `DUMP` | 准备一帧并通过 OV56RGB5 协议发送 RGB565 图像 |
| `SD STATUS` | 只读显示最近一次 SD 操作缓存，不切换硬件状态 |
| `SD SNAPSHOT` | 准备图像并保存为递增命名的 BMP24 文件 |

## 5. 已实现功能

| 功能 | 状态 | 说明 |
|---|---|---|
| OV5640 初始化 | 完成 | 可读取 ID `0x5640` |
| LCD 显示 | 完成 | 支持真实图像显示，LCD ID `0x5310` |
| UART DUMP | 完成 | 输出 160×120 RGB565 和 CRC32 |
| 图像处理 | 完成 | BYPASS、GRAY、BINARY |
| 阈值调节 | 完成 | `THR` 支持 0～255 |
| FreeRTOS 最小多任务 | 完成 | Camera task + monitor |
| 双缓冲 | 完成 | front/back buffer 隔离采集与消费 |
| 运行健康监控 | 完成 | heap、stack、fault、UART DMA、IWDG 状态 |
| SD STATUS | 完成 | 只读缓存状态 |
| SD SNAPSHOT | 完成 | 保存 160×120 BMP24 |
| 文件递增命名 | 完成 | `IMG0001.BMP`～`IMG9999.BMP` |
| 耗时统计 | 完成 | total/prepare/write/cleanup |
| 稳定性测试工具 | 完成 | `tools/uart_sd_snapshot_stability.py` |

## 6. SD Snapshot 最终流程

1. 接收 `SD SNAPSHOT` 命令。
2. 调用 `Camera_RTOS_PrepareRgb565Frame()`。
3. 获取有效 front RGB565 buffer。
4. 复制到 static staging buffer，隔离后续 SD 会话中的 buffer 生命周期。
5. 检查 `source_nonzero` 和 `source_sum32`，空帧不写卡。
6. 保存 OV5640 `0x3018` 原值并 mask `[6:4]`。
7. 将 PC8/PC9/PC10/PC11/PC12/PD2 切换为 SDIO AF12。
8. 执行 polling `HAL_SD_Init()`。
9. 执行 FatFs `f_mount()`。
10. 使用 `f_stat()` 查找第一个可用的 `IMGxxxx.BMP`。
11. 使用 `f_open(FA_CREATE_NEW | FA_WRITE)` 创建文件。
12. 写入 54-byte BMP header。
13. 逐行将 RGB565 转换为 BGR888。
14. 使用 `f_write()` 写入 120 行 BMP 数据。
15. 执行 `f_close()`。
16. 执行 `f_mount(NULL)` 卸载。
17. 执行 `HAL_SD_DeInit()` 并关闭 SDIO clock。
18. 恢复 GPIO/DCMI 配置。
19. 恢复 OV5640 `0x3018` 原值和摄像头链路。
20. 更新 `SD STATUS` 只读缓存。

## 7. 关键技术难点

### 7.1 DVP 与 SDIO 引脚复用冲突

SDIO 初始化和读写失败并不是 SD 卡、卡座、`HAL_SD_Init()` 或 SDIO 硬件本身的问题，而是 OV5640 的 DVP 数据线持续驱动与 SDIO 复用的 GPIO：

- OV_D2 → PC8 → SDIO_D0 → OV5640 `0x3018[4]`。
- OV_D3 → PC9 → SDIO_D1 → OV5640 `0x3018[5]`。
- OV_D4 → PC11 → SDIO_D3 → OV5640 `0x3018[6]`。

最终在 SD 会话前写入 `saved_3018 & 0x8F`，临时关闭 DVP D2/D3/D4 输出并释放共享线；会话结束后恢复原值。

### 7.2 SD-only boot 对照实验

在不启动 OV5640/DVP 的 SD-only boot 环境中，ATK1B 参数下的 1-bit polling 连续读取 block 0 和 block 2048 均 PASS。这一对照实验把 SDIO 硬件链路与摄像头干扰变量分离，证明根因位于外设共享线，而不是 SD 卡或底层读接口。

### 7.3 不能盲目降速解决问题

降低 SDIO 时钟、调整 ClockDiv 或切换 1-bit 只能改变时序裕量，不能消除 OV5640 对共享数据线的主动驱动。因此问题不能按普通信号完整性故障处理，必须先释放总线所有权。

### 7.4 SD SNAPSHOT 图像为空问题

初版 `IMAGE.RGB` 文件大小正确但内容全为 0，说明 FatFs 和 SDIO 写入已成功，故障位于写卡前的数据源。修复时将 DUMP 已验证的 capture/wait/commit/process 逻辑抽成 `Camera_RTOS_PrepareRgb565Frame()`，让 DUMP 和 SD SNAPSHOT 共用同一条图像准备路径，避免依赖尚未产生有效内容的 front buffer。

### 7.5 写卡后恢复图像链路

SD SNAPSHOT 会改变传感器 DVP 输出、GPIO 复用、SDIO clock 和 DCMI 运行状态。成功与失败路径都必须统一执行 file close、unmount、HAL deinit、GPIO/DCMI restore 和 `0x3018` restore；否则后续 DUMP 和 binary image request 会失败。

## 8. 测试结果

1. OV5640 ID 为 `0x5640`。
2. HELP、STATUS 和最终 CLI 命令解析正常。
3. 文本 DUMP 与 binary image request 正常。
4. `SD STATUS` 只读行为正常。
5. `SD SNAPSHOT` 保存 BMP24 正常，单文件大小 57654 bytes。
6. `IMG0001.BMP`、`IMG0002.BMP`、`IMG0003.BMP`、`IMG0004.BMP` 递增正常。
7. 取卡后图片顺序、打开和内容检查正常。
8. SD Snapshot 自动稳定性脚本测试正常，详细 CSV/log 位于 `captures/`，不纳入仓库。
9. 保存后的 DUMP、cleanup 和 restore 正常。
10. `hook_fault=0`、`uart_dma_error=0`、`stream_overflow=0`、IWDG `refresh_skip=0`。

## 9. 性能数据

1. BMP 文件大小：57654 bytes。
2. `total_ms` 典型约 829～1023 ms。
3. 首次保存可能约 1730 ms。
4. `prepare_ms` 约 117～166 ms。
5. `write_ms` 约 555～706 ms，首次可能更高。
6. `cleanup_ms` 约 131～137 ms。

## 10. 当前限制

1. 图像尺寸固定为 160×120。
2. 保存格式固定为 BMP24。
3. 文件编号最多到 `IMG9999.BMP`。
4. SDIO 使用 1-bit polling。
5. 不使用 SDIO DMA 或 IRQ。
6. `SD STATUS` 只读，不主动检测卡。
7. 不支持保存后的文件读回校验。
8. 不支持目录创建。
9. 不支持 RTC 时间戳命名。
10. 每次保存会短暂停止图像链路并切换到 SDIO。

## 11. 后续优化方向

1. 支持 320×240 或更高分辨率保存。
2. 评估 SDIO DMA 写入优化。
3. 支持 BMP、JPEG、RAW 多格式保存。
4. 增加 RTC 时间戳命名和目录分类。
5. 增加 SD 卡空间检测。
6. 增加保存后读回校验。
7. 增加异常断电保护和写入恢复策略。
8. 设计更完整的应用层通信协议。
9. 增加 GUI 或上位机控制界面。
