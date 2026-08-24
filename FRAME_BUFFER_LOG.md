# 摄像头帧缓存 / 双缓冲日志

## 阶段

阶段 4：帧缓存管理 / 双缓冲

## 当前状态

1. STM32F429 + OV5640 + NT35310 LCD 显示链路已完成。
2. 480x320 LCD 雪花问题已解决，FMC/LCD 写时序稳定配置为 6/6。
3. PC Dump 功能已完成，Python 可以发送 `DUMP\n` 并接收 160x120 RGB565 图像。
4. AEC / 曝光调参已完成，当前保持 `OV5640_AEC_TARGET_BASELINE`。
5. AWB / 颜色调试已完成，当前保持 `OV5640_AWB_MODE_AUTO`。
6. 图像参数调试已完成，当前推荐亮度 +1、对比度 0、饱和度 1、锐度 0。
7. 本阶段目标是加入 160x120 RGB565 双缓冲，并接入 PC Dump。

## 当前默认配置

```text
AEC target:
OV5640_AEC_TARGET_BASELINE

AWB mode:
OV5640_AWB_MODE_AUTO

Image parameters:
Brightness = +1
Contrast   = 0
Saturation = 1
Sharpness  = 0
```

## 本阶段目标

1. 新增独立帧缓存管理模块。
2. 使用两个静态 160x120 RGB565 buffer 实现双缓冲。
3. PC Dump 采集链路接入双缓冲。
4. 保持原有 `OV56RGB5` 图像包协议不变。
5. 为后续基础图像处理和 FreeRTOS 多任务化做准备。

## 本阶段不做

1. 不做 320x240 / 480x320 双缓冲。
2. 不使用 SDRAM。
3. 不使用 `malloc/free`。
4. 不修改 OV5640 初始化寄存器表。
5. 不修改 AEC / AWB / 图像参数默认值。
6. 不修改 PC Dump 协议。
7. 不修改 Python 工具。
8. 不做 FreeRTOS。
9. 不做基础图像处理。
10. 不做 SD 卡保存。

## 双缓冲尺寸选择

```text
160 x 120 x 2 bytes = 38,400 bytes
160x120 双缓冲 = 76,800 bytes

320 x 240 x 2 bytes = 153,600 bytes
320x240 双缓冲 = 307,200 bytes

480 x 320 x 2 bytes = 307,200 bytes
480x320 双缓冲 = 614,400 bytes
```

STM32F429 片内 SRAM 不适合直接做 480x320 RGB565 双缓冲，所以本阶段只实现 160x120 RGB565 双缓冲。

## 帧缓存设计

```text
宽度：160
高度：120
格式：RGB565
单像素字节数：2 bytes
单个 buffer 大小：38,400 bytes
buffer 数量：2
总 buffer 大小：76,800 bytes
```

数据流：

```text
DCMI/DMA 采集
    ↓
写入 back buffer
    ↓
Commit / swap
    ↓
front buffer
    ↓
PC Dump 发送
```

当前裸机阶段：

```text
生产者：OV5640 / DCMI / DMA 采集路径
消费者：PC Dump UART 发送路径
```

后续可扩展为：

```text
生产者：摄像头采集任务
消费者：LCD 显示任务 / 图像处理任务 / 推流任务
```

## 修改文件

```text
BSPDrivers/Inc/camera_frame_buffer.h
BSPDrivers/Src/camera_frame_buffer.c
BSPDrivers/Inc/camera_pc_dump.h
BSPDrivers/Src/camera_pc_dump.c
Core/Src/main.c
CMakeLists.txt
FRAME_BUFFER_LOG.md
```

## 实现内容

新增 `camera_frame_buffer` 模块：

```text
1. 定义 160x120 RGB565 双缓冲。
2. 使用两个静态 uint8_t buffer。
3. 提供 front buffer / back buffer 管理。
4. 提供初始化、获取 back buffer、提交 back buffer、获取 front frame 等接口。
5. 不使用 malloc/free。
6. 不使用 SDRAM。
```

核心尺寸：

```text
CAMERA_FB_WIDTH       = 160
CAMERA_FB_HEIGHT      = 120
CAMERA_FB_SIZE_BYTES  = 38,400
CAMERA_FB_COUNT       = 2
```

PC Dump 接入双缓冲后，数据流为：

```text
capture -> back buffer -> commit/swap -> front buffer -> send
```

具体变化：

```text
1. Camera_PC_Dump_GetBufferAddress() 返回 back buffer 地址。
2. DCMI/DMA 将图像写入 back buffer。
3. 采集完成后调用 Camera_FrameBuffer_CommitBackBuffer()。
4. Camera_PC_Dump_SendFrame() 从 front buffer 获取图像数据。
5. OV56RGB5、payload length、CRC32、Python 接收方式保持不变。
```

`main.c` 新增：

```c
#define CAMERA_FRAME_BUFFER_ENABLE       1U
```

并在 PC Dump 流程前初始化：

```c
#if (CAMERA_FRAME_BUFFER_ENABLE != 0U)
    Camera_FrameBuffer_Init();
#endif
```

`CMakeLists.txt` 新增编译源文件：

```text
BSPDrivers/Src/camera_frame_buffer.c
```

## 自检结果

```text
git diff --check 通过。
只有 LF/CRLF 提示，没有 whitespace error。

只发现 160x120 buffer。
没有发现 320x240 / 480x320 双缓冲。

未发现 malloc/free。

OV56RGB5 保持不变。
payload length 保持不变。
CRC32 计算路径保持不变。
Python 工具无需修改。
```

## 编译结果

```text
cmake --build build/Debug 通过。
```

内存占用：

```text
RAM   = 79,200 B / 192 KB
FLASH = 36,360 B / 1 MB
```

RAM 占用主要来自：

```text
160 x 120 x 2 bytes x 2 = 76,800 bytes
```

当前仍在 STM32F429 片内 RAM 可接受范围内。

## 板级测试

测试命令：

```bash
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_fb_round1
```

测试结果：

| Index | Time                | Tag            | Image                                           | Mean brightness | Shadow ratio | Highlight ratio |    R mean |     G mean |    B mean | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----: | ------------------- | -------------- | ----------------------------------------------- | --------------: | -----------: | --------------: | --------: | ---------: | --------: | --------: | --------: | --------: | -----------------: |
|     1 | 2026-07-04T20:41:13 | real_fb_round1 | captures/019_real_fb_round1_20260704_204113.png |      100.293542 |    0.000000% |       8.916667% | 99.121198 | 102.331458 | 92.943698 |  0.968629 |  0.908261 |  0.937677 |         422.088279 |

## 测试结论

1. Python 成功接收 `OV56RGB5` 图像包。
2. PNG 成功保存。
3. `summary.csv` 成功生成 `real_fb_round1` 记录。
4. PC Dump 协议没有被破坏。
5. 双缓冲接入后，PC Dump 主流程可正常工作。
6. Stage 4 Round 1 通过。

## 图像指标说明

```text
mean_brightness = 100.293542，亮度正常。
shadow_ratio = 0.000000%，暗部比例低。
highlight_ratio = 8.916667%，高光比例偏高，但本阶段不继续调曝光。
B/R ratio = 0.937677，在 0.9 ~ 1.15 可接受范围内。
Laplacian variance = 422.088279，低于前面部分测试，但本阶段不以清晰度为优化目标。
```

本阶段重点是验证双缓冲链路，不继续调整画质参数。

## 阶段结论

1. 已完成 160x120 RGB565 双缓冲模块。
2. 已完成 PC Dump 到双缓冲的最小接入。
3. 当前数据流为 `capture -> back buffer -> commit/swap -> front buffer -> send`。
4. 未使用 `malloc/free`。
5. 未使用 SDRAM。
6. 未修改 `OV56RGB5` 协议。
7. 未修改 Python 工具。
8. 未修改 AEC / AWB / 图像参数默认值。
9. Stage 4 可以结束。

## 当前保留配置

```text
Frame buffer:
Width  = 160
Height = 120
Format = RGB565
Count  = 2
Total  = 76,800 bytes

Image tuning:
Brightness = +1
Contrast   = 0
Saturation = 1
Sharpness  = 0

AWB:
OV5640_AWB_MODE_AUTO

AEC:
OV5640_AEC_TARGET_BASELINE
```

## 下一阶段

阶段 5：基础图像处理

建议先做：

```text
1. 基于 front buffer 做灰度统计。
2. 计算平均亮度、最大值、最小值。
3. 可选：二值化测试。
4. 暂不做复杂边缘检测。
5. 暂不做 FreeRTOS。
6. 暂不改 PC Dump 协议。
```