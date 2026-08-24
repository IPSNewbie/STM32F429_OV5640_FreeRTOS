\# PC Dump 调试记录



\## 1. 功能目标



本阶段实现 OV5640 图像数据从 STM32 导出到 PC，用于验证摄像头采集链路，并为后续图像质量调试提供数据依据。



当前项目已经具备两条图像路径：



```text

LCD 实时显示路径：

OV5640 -> DCMI -> DMA -> LCD GRAM -> LCD 480x320 显示



PC Dump 图像导出路径：

OV5640 -> DCMI -> DMA -> SRAM Buffer -> USART1 -> Python/OpenCV

```



其中 PC Dump 功能主要用于：



\* 验证 DCMI/DMA 采集到的图像数据是否正确；

\* 将 RGB565 原始图像导出到 PC；

\* 使用 Python/OpenCV 恢复图像；

\* 生成亮度、颜色、清晰度等图像质量分析结果；

\* 辅助后续 OV5640 曝光、白平衡、锐度等参数调试。



\---



\## 2. 当前实现功能



当前 PC Dump 已实现以下功能：



\* 支持 PC 发送 `DUMP\\n` 命令触发 STM32 采集一帧图像；

\* 支持 160x120 RGB565 图像导出；

\* 支持 TestBar 和 RealImage 两种导出模式；

\* STM32 使用 DCMI Snapshot + DMA 将图像采集到 SRAM Buffer；

\* 通过 USART1 将图像数据发送到 PC；

\* Python 端自动发送 `DUMP\\n` 命令；

\* Python 端接收图像 payload，并进行 CRC 校验；

\* Python 端将 RGB565 转换为 OpenCV 可显示的 BGR888 图像；

\* Python 端保存原图、灰度图、亮度直方图和分析报告；

\* 支持自动创建 `captures/` 文件夹；

\* 支持 `--tag` 参数标记不同测试内容；

\* 支持自动编号和时间戳归档；

\* 支持将每次测试结果追加到 `captures/summary.csv`。



\---



\## 3. 图像格式



当前 PC Dump 图像格式如下：



```text

Resolution: 160x120

Format: RGB565

Pixel size: 2 bytes

Payload length: 38400 bytes

```



计算方式：



```text

160 x 120 x 2 = 38400 bytes

```



其中 RGB565 表示：



```text

R: 5 bit

G: 6 bit

B: 5 bit

```



\---



\## 4. 串口通信方式



当前串口参数：



```text

USART: USART1

Baudrate: 115200

Data format: 8N1

```



PC 端触发命令：



```text

DUMP\\n

```



通信流程：



```text

1\. Python 打开串口；

2\. Python 发送 DUMP\\n；

3\. STM32 收到命令后初始化 OV5640；

4\. STM32 启动 DCMI Snapshot；

5\. DMA 将一帧图像写入 SRAM Buffer；

6\. STM32 发送图像帧头、图像数据和 CRC；

7\. Python 接收数据并校验 CRC；

8\. Python 保存图像和分析报告；

9\. STM32 返回等待下一条 DUMP 命令。

```



\---



\## 5. STM32 端相关宏



PC Dump 模式由 `main.c` 中的宏控制：



```c

\#define CAMERA\_MODE CAMERA\_MODE\_PC\_DUMP\_RGB565

```



TestBar / RealImage 由以下宏控制：



```c

\#define PC\_DUMP\_USE\_REAL\_IMAGE 0U

```



其中：



```text

0U: 导出 OV5640 TestBar

1U: 导出真实图像 RealImage

```



工程默认模式建议保持为：



```c

\#define CAMERA\_MODE CAMERA\_MODE\_480X320\_REAL

```



这样下载程序后默认运行 LCD 480x320 实时显示功能。



\---



\## 6. Python 使用方法



普通使用：



```bash

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200

```



带 tag 使用：



```bash

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag testbar

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_baseline

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_ev\_minus1

```



其中 `COM3` 需要根据设备管理器中的 ST-Link Virtual COM Port 实际端口号修改。



\---



\## 7. Python 输出文件



每次成功采集后，根目录下会更新最新结果：



```text

ov5640\_dump.png

ov5640\_gray.png

ov5640\_hist.png

ov5640\_report.txt

```



同时会在 `captures/` 文件夹中保存归档文件，例如：



```text

captures/001\_real\_baseline\_20260622\_193012.png

captures/001\_real\_baseline\_20260622\_193012\_gray.png

captures/001\_real\_baseline\_20260622\_193012\_hist.png

captures/001\_real\_baseline\_20260622\_193012\_report.txt

captures/summary.csv

```



其中：



\* `.png`：恢复后的原始图像；

\* `\_gray.png`：灰度图；

\* `\_hist.png`：亮度直方图；

\* `\_report.txt`：图像质量分析报告；

\* `summary.csv`：多次测试结果汇总表。



\---



\## 8. 图像质量分析指标



当前 Python 脚本支持以下图像质量分析指标。



\### 8.1 亮度分析



```text

Mean brightness

Min brightness

Max brightness

Shadow ratio

Highlight ratio

```



含义：



\* `Mean brightness`：平均亮度；

\* `Min brightness`：最低亮度；

\* `Max brightness`：最高亮度；

\* `Shadow ratio`：亮度小于 15 的暗部像素比例；

\* `Highlight ratio`：亮度大于 245 的高光像素比例。



判断方法：



```text

Highlight ratio > 5%：可能过曝

Shadow ratio > 20%：可能欠曝

```



\### 8.2 RGB 通道分析



```text

R mean

G mean

B mean

R/G ratio

B/G ratio

B/R ratio

R/B ratio

```



含义：



\* `R mean`：红色通道平均值；

\* `G mean`：绿色通道平均值；

\* `B mean`：蓝色通道平均值；

\* `B/R ratio`：蓝色与红色比例；

\* `R/B ratio`：红色与蓝色比例。



判断方法：



```text

B/R ratio > 1.25：可能偏蓝

R/B ratio > 1.25：可能偏红

```



\### 8.3 清晰度分析



当前使用 Laplacian variance 作为清晰度指标：



```text

Laplacian variance

```



含义：



\* 数值越大，说明边缘信息越丰富，图像越清晰；

\* 数值越小，说明图像越模糊。



当前经验阈值：



```text

Blur threshold: 100.0

```



判断方法：



```text

Laplacian variance < 100：可能模糊

Laplacian variance > 100：清晰度基本正常

```



该指标更适合同一场景下对比不同调参结果，不适合跨场景绝对比较。



\---



\## 9. Baseline 测试结果



RealImage baseline 测试结果如下：



```text

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



分析结果：



```text

1\. 数据长度正确，160 x 120 x 2 = 38400 bytes；

2\. CRC 校验通过，说明串口传输基本正常；

3\. 平均亮度为 106.251，整体亮度处于中等水平；

4\. Shadow ratio 为 0.229%，说明没有明显欠曝；

5\. Highlight ratio 为 8.604%，超过 5%，说明存在一定过曝；

6\. B/R ratio 为 1.113，略微偏蓝但不严重；

7\. Laplacian variance 为 623.007，明显高于 100，说明图像不模糊。

```



当前主要问题：



```text

高光比例偏高，存在一定过曝。

```



\---



\## 10. 当前测试结论



PC Dump 功能已经完成并通过测试：



```text

1\. TestBar 可以正常导出；

2\. RealImage 可以正常导出；

3\. Python 可以正常生成 PNG 图像；

4\. Python 可以正常生成灰度图、直方图和 report；

5\. captures/ 文件夹可以自动保存归档结果；

6\. summary.csv 可以追加多次测试数据；

7\. 默认 480x320 LCD 显示模式未受影响。

```



当前项目已经具备：



```text

OV5640 480x320 LCD 实时显示能力

OV5640 160x120 PC 图像导出能力

PC 端图像质量分析能力

```



\---



\## 11. 下一阶段计划



下一阶段进入 OV5640 图像质量调试。



根据 baseline 结果，优先调试方向为：



```text

曝光 / AEC 参数调试

```



初步目标：



```text

Highlight ratio: 从 8.604% 降低到 3%\~5%

Mean brightness: 保持在 90\~130

B/R ratio: 保持在 0.9\~1.15

Laplacian variance: 不明显下降

```



后续调参建议使用以下方式记录：



```bash

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_baseline

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_ev\_minus1

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_aec\_test1

python tools/pc\_dump\_rgb565.py --port COM3 --baud 115200 --tag real\_gain\_limit

```



每次调参后查看：



```text

captures/summary.csv

```



重点比较：



```text

mean\_brightness

highlight\_ratio

shadow\_ratio

B/R ratio

Laplacian variance

```



通过量化指标判断图像质量是否改善。



