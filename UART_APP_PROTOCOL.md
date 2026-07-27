# UART 应用层通信协议

## 1. 文档目的

本文档用于冻结 STM32F429 + OV5640 项目在 Stage 9 Round 1 的 UART 应用层通信边界。

1. 当前串口使用 COM4、115200 bit/s、8 个数据位、1 个停止位、无校验、无流控。
2. 当前已实现文本 CLI 控制协议和 OV56RGB5 图像二进制响应协议。
3. 当前图像请求仍由文本命令 `DUMP` 触发。
4. Stage 9 后续计划增加二进制图像请求协议；Round 1 仅冻结协议草案，不实现解析。
5. Round 1 将现有 CRC32 算法提取为公共模块，但不改变线上 CRC 结果。
6. 现有 Python 工具 `tools/pc_dump_rgb565.py` 保持兼容且不修改。

## 2. 当前通信架构

```text
PC（MobaXterm / Python）
    ↓
USART1
    ↓
CameraServiceTask
    ↓
文本 CLI 或 DUMP
    ↓
OV56RGB5 图像响应
```

当前实现边界如下：

1. CameraServiceTask 独占 UART、CLI 和 DUMP。
2. UART RX 仍使用 `HAL_UART_Receive()` 单字节轮询。
3. `PENDING` 表示已收到部分命令，CameraServiceTask 必须立即继续轮询。
4. `NONE` 表示本轮没有收到字节，CameraServiceTask 才执行 `osDelay(1U)`。
5. `HAL_TIMEOUT` 不清空已接收的行缓存。
6. `HAL_ERROR` 才清理 UART 错误并重置行缓存。
7. Round 1 未启用 UART RX DMA。
8. Round 1 未启用 USART1 IDLE 中断。
9. Round 1 未使用 StreamBuffer。
10. Round 1 未实现文本/二进制协议分发器。

## 3. 文本 CLI 协议

### 3.1 串口参数

```text
Serial port = COM4
Speed = 115200
Flow control = None
Data bits = 8
Stop bits = 1
Parity = None
Serial engine = PuTTY
```

### 3.2 命令表

| 命令 | 当前含义 | 响应类型 |
|---|---|---|
| `HELP` | 显示命令帮助 | 文本 |
| `STATUS` | 显示图像配置和运行统计 | 文本 |
| `PROC` | 查询当前图像处理模式 | 文本 |
| `PROC BYPASS` | 设置旁路模式 | 文本 |
| `PROC GRAY` | 设置灰度模式 | 文本 |
| `PROC BINARY` | 设置二值化模式 | 文本 |
| `THR` | 查询当前二值化阈值 | 文本 |
| `THR <0-255>` | 设置二值化阈值 | 文本 |
| `RESET` | 恢复 CLI 默认模式和阈值 | 文本 |
| `DUMP` | 请求采集并发送一帧图像 | OV56RGB5 二进制帧 |

命令处理规则：

1. 命令以行结束符结束，兼容 `\n`、`\r` 和 `\r\n`。
2. 命令及参数匹配不区分英文字母大小写。
3. 命令行静态缓存总长为 32 B，最多保存 31 B 有效字符和一个字符串结束符。
4. 普通 CLI 命令返回以换行结束的文本。
5. `DUMP` 是“文本请求、二进制响应”的特殊命令。
6. 收到完整 `DUMP` 后，不发送 `OK`、状态文本或日志，直接进入图像采集和发送流程。
7. OV56RGB5 图像帧之前及发送期间不得混入任何文本。
8. Round 1 保持所有现有文本命令及响应兼容。

## 4. OV56RGB5 图像响应协议

### 4.1 帧布局

线上完整帧为：

```text
22 B 完整 header（包含 8 B magic）
    + 38400 B RGB565 payload
    + 4 B CRC32
    = 38426 B
```

| 绝对偏移 | 字段 | 长度 | 字节序 | 当前值或含义 |
|---:|---|---:|---|---|
| 0 | `magic` | 8 B | ASCII 字节序列 | 固定为 `OV56RGB5` |
| 8 | `version` | 1 B | 不适用 | 固定为 `1` |
| 9 | `format/reserved` | 1 B | 不适用 | 固定为 `1`；固件称保留/图像格式标志，Python 称 `pixel_format` 并校验为 1 |
| 10 | `width` | 2 B | 小端 | 当前为 160 |
| 12 | `height` | 2 B | 小端 | 当前为 120 |
| 14 | `payload_len` | 4 B | 小端 | 当前为 38400 |
| 18 | `frame_id` | 4 B | 小端 | 当前成功发送序号 |
| 22 | `payload` | 38400 B | RGB565 像素按 16 位小端解释 | 160 × 120 × 2 B 图像数据 |
| 38422 | `crc32` | 4 B | 小端 | 仅覆盖 payload 的 CRC-32/ISO-HDLC |

### 4.2 长度与位置

1. magic 长度为 8 B。
2. 完整 header 长度为 22 B，并已包含 magic。
3. payload 起始偏移为 22。
4. payload 长度为 38400 B。
5. CRC 位于 payload 之后，不属于 22 B header。
6. CRC 起始偏移为 38422。
7. 完整帧总长度为 38426 B。
8. 发送顺序保持为 header、payload、CRC32。
9. payload 仍按最多 1024 B 一块进行 UART 发送。

### 4.3 固件与 Python 的术语差异

固件使用 22 B 数组构造包含 magic 的完整 header。Python 先扫描并消费 8 B magic，再按 `<BBHHII` 读取余下 14 B，因此 Python 中的 `HEADER_SIZE` 为 14 B。两端线上布局一致，不能把帧误写成“8 B magic + 22 B header”。

偏移 9 的字段命名也不同：固件注释称“保留（图像格式标识）”，Python 将其命名为 `pixel_format` 并要求值为 1。Round 1 保持当前字节值和两端行为，不修改任何一方。

## 5. frame_id 当前语义

1. `frame_id` 位于完整帧绝对偏移 18。
2. 字段宽度为 4 B，类型为 `uint32_t`，以小端序发送。
3. 生成变量为 `camera_rtos.c` 中的静态变量 `s_camera_rtos_frame_id`。
4. 静态初值为 1，`Camera_RTOS_Init()` 会将其重置为 1。
5. CameraServiceTask 在发送图像时把当前值传给 `Camera_PC_Dump_SendFrame()`。
6. 只有 header、payload 和 CRC 全部发送成功后，`s_camera_rtos_frame_id` 才递增。
7. 发送失败时不递增，后续请求可能复用同一个值。
8. 设备重新初始化或复位后从 1 重新开始；长期运行按 32 位无符号整数自然回绕。
9. 因此它表示“本次启动期间成功发送帧的序号”，不是 DUMP 请求总次数。
10. Python 会读取、显示并记录 `frame_id` 到报告和 `summary.csv`。
11. Python 不校验起始值、连续性、重复、倒退或回绕，也不基于该字段丢弃重复帧。
12. Round 1 不改变 `frame_id` 的任何语义。
13. 后续是否把二进制请求的 `seq` 映射到 `frame_id`，需在兼容性验证后另行决定。

## 6. CRC32 参数

当前固件算法与 Python `zlib.crc32(payload)` 的结果一致。

| 参数 | 当前值 |
|---|---|
| CRC 名称 | CRC-32/ISO-HDLC |
| 宽度 | 32 bit |
| 正向多项式 | `0x04C11DB7` |
| 反射实现多项式 | `0xEDB88320` |
| 初始值 | `0xFFFFFFFF` |
| 输入反射 `RefIn` | true |
| 输出反射 `RefOut` | true |
| 最终异或值 `XorOut` | `0xFFFFFFFF` |
| 校验范围 | 仅 38400 B RGB565 payload |
| 串口字节序 | 32 位小端 |
| 空数据结果 | `0x00000000` |
| 标准测试向量 | ASCII `123456789` |
| 标准测试结果 | `0xCBF43926` |

CRC 不覆盖 magic、version、format/reserved、width、height、payload_len 或 frame_id。公共模块保留逐位反射算法，不使用查找表、动态内存或 STM32 硬件 CRC 外设。

公共接口的增量状态规则：

1. `Protocol_CRC32_Init()` 返回未最终异或的初始状态。
2. `Protocol_CRC32_UpdateByte()` 更新一个字节。
3. `Protocol_CRC32_Update()` 可分块更新任意长度数据。
4. `Protocol_CRC32_Finalize()` 执行最终异或。
5. `Protocol_CRC32_Calculate()` 完成一次性计算。
6. `length == 0` 时结果确定为 `0x00000000`。
7. `data == NULL` 时更新函数不解引用指针并保持当前 CRC 状态；一次性计算会得到空数据结果。
8. 该容错行为无法区分空输入和错误的空指针调用，因此 `length > 0` 时调用方仍必须先校验数据指针。

## 7. 二进制图像请求协议 v1 草案

本节只冻结后续轮次使用的请求格式。目前固件尚未实现该请求的解析、状态机或协议分发，当前 PC 必须继续发送文本 `DUMP\n`。

| 偏移 | 字段 | 长度 | 说明 |
|---:|---|---:|---|
| 0 | `SOF0` | 1 B | 固定为 `0xA5` |
| 1 | `SOF1` | 1 B | 固定为 `0x5A` |
| 2 | `version` | 1 B | 固定为 `0x01` |
| 3 | `msg_type` | 1 B | 固定为 `0x20`，表示 `REQUEST_IMAGE` |
| 4 | `seq` | 2 B | 小端序请求序号 |
| 6 | `payload_len` | 2 B | 小端序，v1 固定为 0 |
| 8 | `payload` | 0 B | v1 无载荷 |
| 8 | `crc32` | 4 B | 小端序 |
| 12 | `EOF0` | 1 B | 固定为 `0x0D` |
| 13 | `EOF1` | 1 B | 固定为 `0x0A` |

草案约束：

1. v1 请求帧总长度为 14 B。
2. CRC 只覆盖 `version`、`msg_type`、`seq` 和 `payload_len`，即偏移 2 至 7 的 6 B。
3. CRC32 算法参数沿用第 6 节的 CRC-32/ISO-HDLC，仅校验范围不同。
4. CRC 不包含 SOF、CRC 字段和 EOF。
5. v1 的 `payload_len` 必须为 0。
6. Round 1 不实现该帧的解析，不新增二进制状态机或协议分发器。
7. 后续实现请求协议后，STM32 图像响应仍使用 OV56RGB5。
8. Round 1 不改变现有 `frame_id`；`seq` 与 `frame_id` 是否映射尚未决定。

## 8. Stage 9 实施轮次

| 轮次 | 计划内容 | 当前状态 |
|---|---|---|
| Round 1 | 协议冻结与 CRC32 公共模块 | 本轮实现 |
| Round 2 | 纯 C 二进制请求状态机 | 未实现 |
| Round 3 | UART DMA + IDLE + StreamBuffer，先迁移文本 CLI | 未实现 |
| Round 4 | 文本/二进制协议分发和图像请求接入 | 未实现 |
| Round 5 | Python 自动化工具和连续请求压力测试 | 未实现 |

后续轮次是计划，不代表当前固件已经具备对应能力。

## 9. Round 1 兼容性约束

1. 串口继续使用 COM4。
2. 文本 CLI 命令和响应不变。
3. `DUMP` 命令及换行触发方式不变。
4. OV56RGB5 magic、22 B header、字段顺序和总长度不变。
5. 现有 Python 工具不变。
6. `frame_id` 生成、递增和复位语义不变。
7. CRC 参数、覆盖范围、结果和串口字节序不变。
8. AEC、AWB、亮度、对比度、饱和度和锐度默认值不变。
9. 默认处理模式仍为 BYPASS，默认二值化阈值仍为 128。
10. `HAL_UART_Receive()` 单字节轮询及 PENDING/NONE 处理不变。
11. Round 1 不实现 DMA、IDLE、StreamBuffer、二进制请求解析器或协议分发器。
12. 不使用 `malloc()` 或 `free()`。

## 10. Round 1 验证

### 10.1 已完成验证

| 验证项 | 结果 | 说明 |
|---|---|---|
| STM32 固件构建 | 通过 | `cmake --build build/Debug` 成功 |
| RAM | 115592 B / 192 KB，58.79% | Debug 构建链接结果 |
| FLASH | 54376 B / 1 MB，5.19% | Debug 构建链接结果 |
| CRC32 主机编译 | 通过 | 使用可用的 Visual Studio C11 编译器，启用严格警告并将警告视为错误 |
| 一次性 CRC 计算 | 通过 | `123456789` 得到 `0xCBF43926` |
| 逐字节 CRC 计算 | 通过 | 与一次性计算一致 |
| 分两段 CRC 计算 | 通过 | 前 4 B 和后 5 B 计算结果一致 |
| 空数据 CRC | 通过 | 得到 `0x00000000` |
| OV56RGB5 静态兼容检查 | 通过 | header 构造、payload、frame_id、CRC 范围和发送顺序未改变 |

当前环境没有可直接调用的主机 `gcc`，因此使用已安装的 Visual Studio C 编译器运行等价主机测试；没有修改系统环境或 STM32 构建配置。

### 10.2 COM4 板上回归测试

板上图像回归测试状态：**待测试**。

MobaXterm 设置：

```text
COM4
115200
8N1
Flow control = None
Serial engine = PuTTY
```

文本命令：

```text
HELP
STATUS
PROC GRAY
STATUS
PROC BINARY
THR 128
STATUS
RESET
STATUS
```

每次运行 Python 前先关闭 MobaXterm，避免两个程序同时占用 COM4。

```bash
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r1_bypass
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r1_gray
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r1_binary
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r1_reset
```

检查每次接收的 magic、尺寸、payload 长度和 CRC，并确认 BYPASS、GRAY、BINARY 和 RESET 后图像符合对应模式。Round 1 不在本文档中编造板上测试结果，也不补录 Stage 8 日志。
