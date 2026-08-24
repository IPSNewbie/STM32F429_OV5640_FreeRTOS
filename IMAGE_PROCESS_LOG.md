# 基础图像处理日志

## 阶段

阶段 5：基础图像处理

## 当前状态

1. STM32F429 + OV5640 + NT35310 LCD 显示链路已完成。
2. PC Dump 功能已完成，Python 可以接收 160x120 RGB565 图像。
3. AEC / AWB / 图像参数调试已完成。
4. 160x120 RGB565 双缓冲已完成。
5. 当前阶段目标是在 STM32 端完成基础图像处理。

## 当前默认配置

```text
AEC:
OV5640_AEC_TARGET_BASELINE

AWB:
OV5640_AWB_MODE_AUTO

Image tuning:
Brightness = +1
Contrast   = 0
Saturation = 1
Sharpness  = 0

Frame buffer:
Width  = 160
Height = 120
Format = RGB565
Count  = 2
```

## 本阶段目标

1. 新增基础图像处理模块。
2. 支持 BYPASS / GRAYSCALE / BINARY 三种模式。
3. 支持 RGB565 转灰度 RGB565。
4. 支持 RGB565 转二值 RGB565。
5. 支持灰度统计。
6. 保持 PC Dump 协议不变。
7. 处理结果继续通过 Python PC Dump 验证。

## 本阶段不做

1. 不做 Sobel / Canny。
2. 不做人脸识别。
3. 不移植 OpenCV。
4. 不做 LCD 实时显示处理结果。
5. 不做 UART CLI。
6. 不做 FreeRTOS。
7. 不做 SD 卡保存。
8. 不修改 OV56RGB5 协议。
9. 不修改 Python 工具。
10. 不修改 AEC / AWB / 图像参数默认值。

## 图像处理数据流

原始采集：

```text
DCMI/DMA -> back buffer -> commit -> front buffer
```

图像处理：

```text
front buffer 原图 -> image process -> back buffer 处理图 -> commit -> front buffer 处理图
```

PC Dump：

```text
front buffer 处理图 -> OV56RGB5 packet -> UART -> Python
```

## 处理模式

```text
BYPASS:
不改变图像内容，用于验证新增模块不破坏原始 PC Dump。

GRAYSCALE:
RGB565 原图转灰度，再重新打包成 RGB565 输出。

BINARY:
RGB565 原图转灰度，再按 threshold 输出黑白 RGB565。
```

## 灰度算法

RGB565 拆分为 R/G/B 后，扩展到 8 bit。

灰度计算使用整数近似：

```text
gray = (77 * R + 150 * G + 29 * B) >> 8
```

不使用 float/double。

## 二值化算法

```text
gray >= threshold -> white = 0xFFFF
gray <  threshold -> black = 0x0000
```

默认阈值：

```text
threshold = 128
```

## 统计信息

每次处理记录：

```text
pixel_count
gray_sum
gray_min
gray_max
shadow_count
highlight_count
threshold
```

阈值：

```text
shadow: gray < 30
highlight: gray > 240
```

## Round 1 默认设置

```c
#define CAMERA_IMAGE_PROCESS_ENABLE        1U

#define CAMERA_PROCESS_MODE_BYPASS         0
#define CAMERA_PROCESS_MODE_GRAYSCALE      1
#define CAMERA_PROCESS_MODE_BINARY         2

#define CAMERA_PROCESS_MODE                CAMERA_PROCESS_MODE_BYPASS
#define CAMERA_BINARY_THRESHOLD            128U
```

## 测试方法

BYPASS 测试：

```bash
python tools/pc_dump_rgb565.py --port COM5 --baud 115200 --tag imgproc_bypass
```

灰度图测试：

```bash
python tools/pc_dump_rgb565.py --port COM5 --baud 115200 --tag imgproc_gray
```

二值图测试：

```bash
python tools/pc_dump_rgb565.py --port COM5 --baud 115200 --tag imgproc_binary
```

## 测试结果

| Test item  | Mode      | Tag            | Image                                           | Mean brightness | Shadow ratio | Highlight ratio |     R mean |     G mean |     B mean | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ---------- | --------- | -------------- | ----------------------------------------------- | --------------: | -----------: | --------------: | ---------: | ---------: | ---------: | --------: | --------: | --------: | -----------------: |
| Bypass     | BYPASS    | imgproc_bypass | captures/020_imgproc_bypass_20260704_231125.png |      103.402135 |    0.000000% |       8.718750% | 102.175729 | 105.871510 |  94.002292 |  0.965092 |  0.887890 |  0.920006 |         479.397575 |
| Grayscale  | GRAYSCALE | imgproc_gray   | captures/021_imgproc_gray_20260704_231209.png   |      102.219896 |    0.098958% |       9.130208% | 101.907344 | 102.502448 | 101.907344 |  0.994194 |  0.994194 |  1.000000 |         529.717420 |
| Binary 128 | BINARY    | imgproc_binary | captures/022_imgproc_binary_20260704_231311.png |       72.953906 |   71.390625% |      28.609375% |  72.953906 |  72.953906 |  72.953906 |  1.000000 |  1.000000 |  1.000000 |        7982.491684 |

## 结果分析

### BYPASS

1. Python 成功接收图像。
2. PNG 成功保存。
3. summary.csv 成功生成记录。
4. 图像处理模块接入后，原始 PC Dump 没有被破坏。
5. BYPASS 模式验证通过。

### GRAYSCALE

1. 输出图像变为灰度图。
2. `B/R ratio = 1.000000`。
3. `R/G ratio = 0.994194`，`B/G ratio = 0.994194`。
4. R、G、B 三通道基本一致。
5. RGB565 灰度化成功。

G 通道与 R/B 略有差异，是 RGB565 中 G 通道为 6 bit、R/B 通道为 5 bit 导致的正常量化差异。

### BINARY

1. 输出图像变为黑白图。
2. `R/G ratio = 1.000000`。
3. `B/G ratio = 1.000000`。
4. `B/R ratio = 1.000000`。
5. `shadow_ratio = 71.390625%`。
6. `highlight_ratio = 28.609375%`。
7. `shadow_ratio + highlight_ratio = 100.000000%`。

这说明二值化输出只包含黑色和白色，阈值分割生效。

`Laplacian variance = 7982.491684` 很高，是黑白边界突变导致的正常结果。

## 阶段结论

1. 已完成 `camera_image_process` 基础图像处理模块。
2. 已支持 BYPASS 模式。
3. 已支持 RGB565 灰度化。
4. 已支持 RGB565 二值化。
5. 已支持灰度统计信息记录。
6. PC Dump 协议未修改。
7. Python 工具未修改。
8. 未使用 malloc/free。
9. 未使用 SDRAM。
10. 未进入 FreeRTOS。
11. Stage 5 Round 1 通过。

## 当前建议默认配置

阶段 5 完成后，默认处理模式建议保持 BYPASS：

```c
#define CAMERA_IMAGE_PROCESS_ENABLE        1U
#define CAMERA_PROCESS_MODE                CAMERA_PROCESS_MODE_BYPASS
#define CAMERA_BINARY_THRESHOLD            128U
```

原因：

1. BYPASS 保持正常 PC Dump 输出原图。
2. GRAYSCALE 和 BINARY 作为可切换测试模式保留。
3. 后续进入 CLI 阶段后，再通过串口命令动态切换处理模式。

## 当前保留能力

```text
采集能力：
OV5640 -> DCMI/DMA -> 双缓冲

输出能力：
front buffer -> OV56RGB5 -> UART -> Python PNG

处理能力：
BYPASS
GRAYSCALE
BINARY threshold 128
灰度统计
```

## 下一阶段

阶段 6：串口 CLI 在线调参

建议内容：

1. 新增简单串口命令解析。
2. 支持查询当前模式。
3. 支持切换图像处理模式。
4. 支持设置二值化阈值。
5. 暂不改 PC Dump 协议。
6. 暂不进入 FreeRTOS。