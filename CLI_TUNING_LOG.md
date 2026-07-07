# Stage 6 串口 CLI 在线调参日志

## 目标

新增最小串口 CLI，用于在线查询状态、切换图像处理模式、设置二值化阈值，同时保持 `DUMP` 和 `OV56RGB5` 图像包协议不变。

## 默认值

```text
process mode = BYPASS
binary threshold = 128
AEC = OV5640_AEC_TARGET_BASELINE
AWB = OV5640_AWB_MODE_AUTO
Brightness = +1
Contrast = 0
Saturation = 1
Sharpness = 0
```

## 支持命令

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

## 串口工具结论

```text
旧串口助手会触发 CH340 一键下载相关电路，使 STM32 进入 0x1FFFxxxx System Bootloader。
这不是 PC Dump、HAL_UART_Receive、line buffer 或图像处理问题。
```

推荐工具：

```text
MobaXterm Serial
Serial port = COM6
Speed = 115200
Flow control = None
```

## 测试结果

```text
STATUS 后关闭 MobaXterm，再用 Python DUMP：成功。
PROC GRAY 后 DUMP：成功。
PROC BINARY + THR 128 后 DUMP：成功。
RESET 后 BYPASS DUMP：成功。
```

记录：

```text
GRAY:
tag = moba_gray_dump
mean_brightness = 102.587708
shadow_ratio = 0
highlight_ratio = 10.880208
B/R ratio = 1
laplacian_variance = 510.336961

BINARY:
tag = moba_reset_bypass_dump
note = tag 误写，实际为 BINARY 测试
mean_brightness = 81.892188
shadow_ratio = 67.885417
highlight_ratio = 32.114583
B/R ratio = 1
laplacian_variance = 5425.523438

RESET / BYPASS:
tag = moba_reset_bypass_dump
mean_brightness = 104.483021
shadow_ratio = 0
highlight_ratio = 10.427083
B/R ratio = 1.086039
laplacian_variance = 437.839114
```

## 阶段结论

Stage 6 通过。
