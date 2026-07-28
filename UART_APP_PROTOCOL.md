# UART 应用层通信协议

## 1. 文档目的

本文档用于冻结 STM32F429 + OV5640 项目的 UART 应用层通信边界，并记录 Stage 9 各轮次的兼容性约束。

1. 当前串口使用 COM4、115200 bit/s、8 个数据位、1 个停止位、无校验、无流控。
2. 当前已实现文本 CLI 控制协议和 OV56RGB5 图像二进制响应协议。
3. 图像请求可由文本命令 `DUMP` 或 v1 二进制 `REQUEST_IMAGE` 触发。
4. Round 4 已接入文本/二进制协议分发，二进制响应仍使用既有 OV56RGB5。
5. Round 1 将现有 CRC32 算法提取为公共模块，但不改变线上 CRC 结果。
6. 现有 Python 工具 `tools/pc_dump_rgb565.py` 保持兼容且不修改。
7. Round 2 新增纯 C 二进制图像请求解析器。
8. Round 3 已将接收链路迁移到 UART DMA + IDLE + 静态 StreamBuffer；Round 4 在任务上下文接入协议分发和图像请求。

## 2. 当前通信架构

```text
PC（MobaXterm / Python）
    ↓
USART1
    ↓
Circular RX DMA（DMA2 Stream2 Channel 4）
    ↓ IDLE / HT / TC 位置差分
HAL_UARTEx_RxEventCallback
    ↓ xStreamBufferSendFromISR
静态 StreamBuffer
    ↓ xStreamBufferReceive
CameraServiceTask
    ↓
camera_uart_dispatcher
    ├── TEXT_BYTE → 文本 CLI / DUMP
    └── IMAGE_REQUEST → 既有 DUMP 路径 → OV56RGB5
```

当前实现边界如下：

1. CameraServiceTask 独占 UART、CLI 和 DUMP。
2. UART RX 使用 `HAL_UARTEx_ReceiveToIdle_DMA()` 启动 Circular DMA，正常运行期间只启动一次。
3. IDLE、DMA HT 和 DMA TC 事件通过当前 DMA 写入位置与旧位置的差分提取新增字节。
4. ISR 只搬运新增字节到静态 StreamBuffer、维护最小统计并按需唤醒任务。
5. CameraServiceTask 使用 `xStreamBufferReceive()` 阻塞等待数据，并逐字节送入 `camera_uart_dispatcher`。
6. 分发器把文本字节送入现有文本行解析器，把二进制候选帧送入 `image_request_protocol`。
7. UART 错误只在 ISR 中记录并置位，DMA 的中止、清错和重新启动在 CameraServiceTask 上下文完成。
8. StreamBuffer 溢出后，CameraServiceTask 放弃本地剩余字节、同时复位文本和二进制解析状态并排空缓冲区。
9. 文本 `DUMP` 仍由 CameraServiceTask 串行执行，响应前不发送文本，图像响应仍为 OV56RGB5。
10. 合法二进制 `REQUEST_IMAGE` 复用文本 `DUMP` 的采集、处理、发送和统计路径。
11. 二进制解析错误和超时只记录统计，不输出文本，不触发 DUMP。

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
13. 二进制请求的 `seq` 与 OV56RGB5 的 `frame_id` 相互独立，不进行映射。

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

## 7. 二进制图像请求协议 v1

本节冻结二进制请求格式。Round 2 已实现独立纯 C 解析器，Round 4 已通过协议分发器接入 CameraServiceTask 和既有 DUMP 路径；文本 `DUMP\n` 继续兼容。

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

协议约束：

1. v1 请求帧总长度为 14 B。
2. CRC 只覆盖 `version`、`msg_type`、`seq` 和 `payload_len`，即偏移 2 至 7 的 6 B。
3. CRC32 算法参数沿用第 6 节的 CRC-32/ISO-HDLC，仅校验范围不同。
4. CRC 不包含 SOF、CRC 字段和 EOF。
5. v1 的 `payload_len` 必须为 0。
6. Round 2 只实现独立解析状态机，实际 UART 协议分发由 Round 4 接入。
7. STM32 图像响应仍使用 OV56RGB5，不新增二进制响应格式。
8. 请求 `seq` 只标识 PC 发出的请求，响应 `frame_id` 仍按固件成功发送帧的既有规则递增。

## 8. Stage 9 实施轮次

| 轮次 | 计划内容 | 当前状态 |
|---|---|---|
| Round 1 | 协议冻结与 CRC32 公共模块 | 已完成 |
| Round 2 | 纯 C 二进制请求状态机 | 已完成 |
| Round 3 | UART DMA + IDLE + StreamBuffer，先迁移文本 CLI | 已完成并通过 COM4 板上测试 |
| Round 4 | 文本/二进制协议分发和图像请求接入 | 编码与主机验证完成，COM4 板上测试待进行 |
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

## 11. 二进制图像请求解析器

### 11.1 模块边界

Round 2 新增 `image_request_protocol` 纯 C 模块：

1. 解析器由调用方静态分配，不使用动态内存。
2. 解析器不依赖 HAL、FreeRTOS、CMSIS、CameraServiceTask 或系统时钟。
3. 输入方式为逐字节调用 `ImageRequestProtocol_FeedByte()`。
4. 当前时间由调用方通过 `now_ms` 参数传入。
5. v1 请求帧固定为 14 B，`payload_len` 只允许为 0。
6. CRC 使用 Round 1 的公共模块，只覆盖偏移 2 至 7 的 6 B。
7. 支持随机前缀过滤、错误重同步、连续帧和 100 ms 半帧超时。
8. 超时差值使用无符号减法，支持 `uint32_t` tick 回绕。
9. Round 2 本轮结束时尚未接入实际 UART、CameraServiceTask 或 DUMP，也未做 COM4 协议实测。
10. Round 3 完成 UART RX 迁移，Round 4 已接入协议分发和图像请求。

### 11.2 状态机

```text
SYNC0
  ↓ A5
SYNC1
  ↓ 5A
VERSION
  ↓
MSG_TYPE
  ↓
SEQ_LOW
  ↓
SEQ_HIGH
  ↓
LEN_LOW
  ↓
LEN_HIGH
  ↓
CRC0
  ↓
CRC1
  ↓
CRC2
  ↓
CRC3
  ↓
EOF0
  ↓
EOF1
  ↓
OK / ERROR / RESYNC
```

解析规则：

1. `SYNC0` 忽略非 `0xA5` 字节；收到 `0xA5` 后进入候选帧。
2. `SYNC1` 收到 `0x5A` 后初始化 CRC；再次收到 `0xA5` 时把后一个字节视为新的 SOF0。
3. version 必须为 `0x01`，msg_type 必须为 `0x20`。
4. seq、payload_len 和 received CRC 均按小端序拼接。
5. 只有收到 LEN_HIGH 后才校验 `payload_len == 0`。
6. 两个 EOF 字节均正确后才最终化并比较 CRC，因此 EOF 错误优先于 CRC 错误。
7. 只有返回 `OK` 时才写入独立的 `out_frame` 对象，随后解析器自动复位。

### 11.3 解析结果

| 结果 | 含义 |
|---|---|
| `NONE` | 尚未进入有效候选帧 |
| `PENDING` | 候选帧尚未接收完整 |
| `OK` | 合法请求帧 |
| `CRC_ERROR` | CRC 错误 |
| `VERSION_ERROR` | 版本错误 |
| `TYPE_ERROR` | 消息类型错误 |
| `LENGTH_ERROR` | 长度错误 |
| `EOF_ERROR` | 帧尾错误 |
| `TIMEOUT` | 半帧超时 |
| `BAD_ARGUMENT` | 参数错误 |

### 11.4 重同步与超时

1. 字段、长度或帧尾错误后，解析器清除旧候选帧并继续搜索下一帧。
2. 当前错误字节为 `0xA5` 时，该字节被保留为下一候选帧的新 SOF0，状态进入 `SYNC1`。
3. 当前错误字节不是 `0xA5` 时，解析器回到 `SYNC0`。
4. `A5 A5 5A` 会从第二个 `A5` 正确同步。
5. 随机垃圾字节不产生字段错误，后续合法帧仍可成功。
6. 每收到一个候选帧字节都会更新 `last_byte_time_ms`。
7. 活动候选帧满足 `(uint32_t)(now_ms - last_byte_time_ms) >= 100U` 时返回 `TIMEOUT`。
8. `FeedByte()` 检测到旧帧超时时，使用当前新字节执行重同步；当前字节为 `0xA5` 时仍保留为新 SOF0。
9. 快速重同步保留 `0xA5` 时 `IsActive()` 返回 1；普通成功、错误或 `CheckTimeout()` 超时复位后返回 0。

### 11.5 Round 2 验证

| 验证项 | 结果 | 说明 |
|---|---|---|
| 固定请求帧 CRC | 通过 | CRC 输入 `01 20 34 12 00 00`，结果 `0xDBB445EA` |
| 固定 14 B 请求帧 | 通过 | `A5 5A 01 20 34 12 00 00 EA 45 B4 DB 0D 0A` |
| 主机测试 | 通过 | 总数 47，通过 47，失败 0，退出码 0 |
| 主机编译器 | 通过 | Visual Studio C11，使用 `/std:c11 /W4 /WX /utf-8` |
| STM32 固件构建 | 通过 | `image_request_protocol.c` 已进入固件编译 |
| RAM | 115592 B / 192 KB，58.79% | Debug 链接结果 |
| FLASH | 54376 B / 1 MB，5.19% | 模块尚未被业务代码引用，未引用函数段被链接器移除 |
| HAL / FreeRTOS 依赖扫描 | 通过 | 解析器和主机测试均无相关依赖 |
| UART 接收链路 | Round 2 时未修改 | Round 3 已迁移到 DMA + IDLE + StreamBuffer |
| COM4 板上协议测试 | 未执行 | Round 2 尚未接入真实 UART |

## 12. UART DMA 接收与文本 CLI 迁移

### 12.1 Round 3 范围

1. USART1 RX 已迁移为 Circular DMA + Receive-to-IDLE。
2. DMA 使用 CubeMX 生成的 DMA2 Stream2 Channel 4。
3. Round 3 只迁移既有文本 CLI 和文本 `DUMP`。
4. Round 3 尚未把 `image_request_protocol` 接入 UART。
5. 文本/二进制协议分发器由 Round 4 新增。
6. 未修改 UART TX；CLI 文本和 OV56RGB5 图像仍使用阻塞发送。

### 12.2 DMA 与 StreamBuffer

1. DMA 接收缓冲区为 128 B。
2. StreamBuffer 有效容量为 512 B，使用 `xStreamBufferCreateStatic()` 创建。
3. 静态存储区额外保留一个 FreeRTOS 环形缓冲空槽，不使用动态内存。
4. IDLE、HT 和 TC 事件均保留。
5. `size > old_position` 时搬运单段新增区域。
6. `size < old_position` 时按缓冲区末尾和开头两段搬运。
7. `size == old_position` 时不重复搬运。
8. `size == 128` 时保留边界位置，用于过滤 TC 后紧接的同位置 IDLE 重复事件；下一次回绕事件再按两段处理。

### 12.3 ISR 职责边界

ISR 只执行以下工作：

1. 确认回调属于已保存的 USART1 句柄。
2. 根据 DMA 位置差分确定新增字节。
3. 使用 `xStreamBufferSendFromISR()` 搬运字节。
4. 维护事件数、接收字节数、写入字节数和溢出字节数。
5. UART 错误回调只增加错误计数并设置恢复标志。
6. 使用 `portYIELD_FROM_ISR()` 按需唤醒 CameraServiceTask。

ISR 不执行 UART TX、CLI、DUMP、图像采集、协议重同步或日志打印。

### 12.4 CameraServiceTask 流程

1. 任务启动时静态创建 StreamBuffer，并启动一次 Receive-to-IDLE DMA。
2. 每次最多从 StreamBuffer 读取 32 B，最长阻塞等待 100 ms。
3. Round 3 的每个字节继续使用 `camera_pc_dump` 中的现有文本行状态机。
4. 完整 CLI 命令仍交给 `Camera_CLI_HandleLine()`。
5. 完整 `DUMP` 仍由 CameraServiceTask 执行采集、处理和 OV56RGB5 发送。
6. DUMP 期间 RX DMA 继续接收，但 CameraServiceTask 暂停解析新命令。
7. DUMP 完成后继续处理本地读取块和 StreamBuffer 中的后续字节；若发生溢出则放弃残缺数据并重新同步。

### 12.5 错误与溢出恢复

1. UART 错误 ISR 不执行复杂恢复。
2. CameraServiceTask 调用 `HAL_UART_AbortReceive()`，清除 UART 错误并重新启动一次 Receive-to-IDLE DMA。
3. 恢复失败时保留恢复请求，后续继续重试，不解析旧数据。
4. StreamBuffer 写入不足时记录丢失字节并设置溢出标志。
5. Round 3 的 CameraServiceTask 检测到溢出后清除文本行状态、丢弃当前读取块剩余字节并非阻塞排空 StreamBuffer。
6. 只有确认 StreamBuffer 已排空后才清除溢出标志并记录重同步次数。
7. 溢出或恢复过程不输出文本，避免污染 OV56RGB5 二进制响应。

### 12.6 CMSIS-RTOS2 与原生 FreeRTOS API 边界

1. 任务创建和任务延时继续使用 CMSIS-RTOS2。
2. 字节流缓冲使用 FreeRTOS StreamBuffer API。
3. ISR 使用 FreeRTOS `FromISR` API；USART1 和 DMA2 Stream2 IRQ 优先级均为 5，符合当前 FreeRTOS 系统调用优先级边界。
4. MonitorTask 不访问 UART 或 StreamBuffer。

### 12.7 当前验证边界

1. 固件构建和静态检查由 Round 3 代码验证覆盖。
2. Round 3 的文本 CLI、文本 DUMP、错误恢复和溢出重同步已通过 COM4 板上测试。
3. Round 3 未接入二进制请求；Round 4 的二进制请求仍需单独完成 COM4 板上验证。

## 13. 文本与二进制协议分发

### 13.1 模块边界

Round 4 新增纯 C 模块 `camera_uart_dispatcher`。该模块不依赖 HAL、CMSIS-RTOS、FreeRTOS、StreamBuffer、CLI 或 UART 句柄，不执行 UART 发送、图像采集和 DUMP，也不使用动态内存。时间戳由 CameraServiceTask 通过 `now_ms` 传入。

当前数据流为：

```text
DMA + IDLE
    ↓
StreamBuffer
    ↓
CameraServiceTask
    ↓
camera_uart_dispatcher
    ├── TEXT_BYTE → 现有 CLI / 文本 DUMP
    └── IMAGE_REQUEST → 现有 DUMP → OV56RGB5
```

### 13.2 分发模式

| 模式 | 含义 | 退出条件 |
|---|---|---|
| `IDLE` | 当前没有文本行或二进制候选帧 | 非 `0xA5` 字节进入文本；`0xA5` 进入二进制候选 |
| `TEXT` | 当前正在接收文本行 | 仅收到 LF（`0x0A`）后回到 `IDLE` |
| `BINARY` | 当前正在接收二进制候选帧，或隔离错误帧的剩余尾部 | 解析完成或尾部隔离结束后回到 `IDLE` |

规则如下：

1. `IDLE` 收到 `0xA5` 时，该字节只进入二进制解析器，不进入文本解析器。
2. `IDLE` 收到其他字节时产生 `TEXT_BYTE`；LF 后保持 `IDLE`，其他字节进入 `TEXT`。
3. 文本行开始后，直到 LF 前都不切换协议；文本中的 `0xA5` 仍是文本字节。
4. CR 不是分发层的结束条件；正常 `\r\n` 在 LF 后回到 `IDLE`。
5. 正常 `BINARY` 字节只进入 `image_request_protocol`；尾部隔离期间的字节只被丢弃，二者均不进入现有文本行解析器。
6. `A5 00` 作为普通帧头搜索失败静默回到 `IDLE`，不产生二进制错误事件。
7. `A5 A5 5A` 从第二个 `A5` 重新同步并继续接收候选帧。
8. 解析错误字节本身为 `0xA5` 时，解析器把它保留为新 SOF0，分发器继续保持 `BINARY`。
9. 尾部隔离不是新的公开模式，而是 `BINARY` 的内部子状态。

### 13.3 分发结果与业务动作

| 分发结果 | CameraServiceTask 动作 |
|---|---|
| `NONE` | 不执行协议业务动作 |
| `TEXT_BYTE` | 把当前字节送入 `Camera_PC_Dump_FeedCommandByte()` |
| `IMAGE_REQUEST` | 记录合法请求和 seq，调用文本 DUMP 共用的发送路径 |
| `BINARY_ERROR` | 按 CRC、version、type、length 或 EOF 分类计数，不输出文本 |
| `BINARY_TIMEOUT` | 增加半帧超时计数，不输出文本，不触发 DUMP |
| `BAD_ARGUMENT` | 记录内部状态错误，不修改分发器有效状态 |

合法固定请求帧为：

```text
A5 5A 01 20 34 12 00 00 EA 45 B4 DB 0D 0A
```

其中请求 `seq = 0x1234`。解析成功后只触发一次既有 DUMP 路径，直接返回 OV56RGB5；响应前不发送 `OK`、ACK、STATUS 或日志。

### 13.4 错误、超时与输入恢复

1. CRC、version、type、length 和 EOF 错误均不触发 DUMP，也没有文本错误响应。
2. Round 4 不定义 ACK、NACK、重传或新的二进制错误响应帧。
3. 二进制候选帧超时阈值保持为 100 ms；CameraServiceTask 的 StreamBuffer 读取无数据时调用分发器超时检查。
4. `FeedByte()` 发现旧候选帧超时时，当前字节按底层解析器既有规则参与重同步，不进行二次回灌。
5. UART DMA 错误恢复成功或 StreamBuffer 溢出重同步时，同时复位分发器和现有文本命令解析器。
6. version、type、length 或 EOF0 提前报错时，分发器先且只产生一次 `BINARY_ERROR`。
7. 若错误字节 `0xA5` 已被底层保留为新 SOF0，则继续正常 `BINARY`，不进入尾部隔离。
8. 底层解析器 inactive 时，按错误前状态隔离固定剩余字节：VERSION 为 11 B、MSG_TYPE 为 10 B、LEN_HIGH 为 6 B、EOF0 为 1 B，EOF1 和 CRC 错误为 0 B。
9. 隔离期间所有输入字节，包括 LF 和 `0xA5`，只被丢弃并递减剩余计数，不产生 `TEXT_BYTE`、新错误或图像请求，也不重放最后一个字节。
10. 隔离时间戳在建立隔离和每次丢弃字节时更新；剩余计数清零后回到 `IDLE`。
11. 错误帧被截断时，尾部隔离在 100 ms 后静默解除并返回 `NONE`，不重复产生 `BINARY_ERROR`，也不产生 `BINARY_TIMEOUT`。
12. `CameraUartDispatcher_Reset()` 会同时清除底层解析器和全部尾部隔离字段。

### 13.5 seq 与 frame_id

1. 请求 `seq` 只标识 PC 发出的二进制请求，当前仅用于运行统计。
2. OV56RGB5 `frame_id` 只标识 STM32 成功发送的图像帧，继续由 `s_camera_rtos_frame_id` 管理。
3. 请求 `seq = 0x1234` 时，响应 `frame_id` 不要求等于 `0x1234`。
4. 现有 Python 工具读取 `frame_id`，不知道请求 `seq`，且本轮保持不变。
5. 后续若需要请求与响应关联，应扩展协议版本，不能改变既有 `frame_id` 字段语义。

### 13.6 当前验证边界

1. Round 4 分发器使用 Visual Studio C11 编译器和 `/std:c11 /W4 /WX /utf-8` 进行独立主机测试。
2. 原 CRC32 主机测试和 47 项图像请求解析器测试继续作为回归项目。
3. 固件构建继续验证分发器、CameraServiceTask 和 STATUS 集成。
4. Round 4 尚未新增正式 Python 二进制请求工具，也未执行连续请求压力测试。
5. Round 4 编码完成后仍需使用 COM4 进行板上测试；Round 5 才新增正式 Python 工具和压力测试。

当前验证结果：

| 验证项 | 结果 |
|---|---|
| CRC32 主机回归 | 通过，`123456789` 为 `0xCBF43926` |
| image_request_protocol 主机回归 | 47/47 通过 |
| camera_uart_dispatcher 主机测试 | 85/85 通过，保留原 62 项并新增 23 项尾部隔离测试 |
| STM32 Debug 固件构建 | 通过 |
| RAM | 116488 B / 192 KB，59.25% |
| CCMRAM | 0 B / 64 KB，0.00% |
| FLASH | 66128 B / 1 MB，6.31% |
| COM4 二进制请求板上测试 | 待测试 |
