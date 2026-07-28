# UART 文本与二进制协议分发日志

## 阶段

Stage 9 Round 4：文本/二进制协议分发与图像请求接入

## 当前基础

1. CRC32 公共模块已完成，算法与既有 OV56RGB5 保持一致。
2. 14 B 二进制图像请求解析器已完成，原 47 项主机测试通过。
3. USART1 RX 已迁移为 DMA + IDLE + 静态 StreamBuffer。
4. CameraServiceTask 独占 UART、CLI 和 DUMP。
5. Round 3 已通过 COM4 板上测试。
6. 本轮把二进制 `REQUEST_IMAGE` 接入 CameraServiceTask，图像响应继续使用 OV56RGB5。

## 本轮目标

1. 新增纯 C 文本/二进制协议分发器。
2. 保持现有文本 CLI 和文本 `DUMP` 兼容。
3. 合法二进制图像请求复用现有 DUMP 路径。
4. 二进制错误和超时只记录统计，不发送文本。
5. 保持 UART DMA、StreamBuffer、OV56RGB5 和现有 Python 工具不变。

## 模块结构

```text
uart_rx_dma
    负责 USART1 Circular DMA、IDLE/HT/TC 事件和静态 StreamBuffer。

camera_uart_dispatcher
    负责区分 TEXT 与 BINARY、隔离错误帧尾部并输出分发事件。

image_request_protocol
    负责解析 14 B REQUEST_IMAGE 请求和 100 ms 半帧超时。

camera_pc_dump
    负责现有文本命令解析、图像缓冲接口和 OV56RGB5 发送。

camera_rtos
    在 CameraServiceTask 中读取字节、处理分发事件并执行 DUMP。
```

## 分发模式

### IDLE

当前没有文本行或二进制候选帧。收到 `0xA5` 时进入 BINARY；收到其他字节时产生 `TEXT_BYTE`，非 LF 字节使模式进入 TEXT。

### TEXT

所有字节均作为 `TEXT_BYTE` 转发，包括 `0xA5`。只有 LF 使分发器回到 IDLE，保证文本行中途不切换协议。

### BINARY

正常候选帧字节只进入 `image_request_protocol`。提前字段错误且底层解析器 inactive 时，分发器在 BINARY 内部进入尾部隔离子状态；隔离期间的字节只被静默丢弃，不进入文本或二进制解析器。该子状态不增加新的公开分发模式。

## 文本路径

```text
StreamBuffer 字节
    ↓
camera_uart_dispatcher
    ↓ TEXT_BYTE
Camera_PC_Dump_FeedCommandByte()
    ↓
HELP / STATUS / PROC / THR / RESET / DUMP
```

文本命令和响应格式不变。文本 `DUMP` 仍不输出前导文本，直接进入图像采集和 OV56RGB5 发送。

## 二进制路径

```text
StreamBuffer 字节
    ↓
camera_uart_dispatcher
    ↓ BINARY
image_request_protocol
    ↓ IMAGE_REQUEST
Camera_RTOS_ProcessDumpRequest()
    ↓
采集 → 双缓冲提交 → 图像处理 → OV56RGB5
```

合法固定请求帧：

```text
A5 5A 01 20 34 12 00 00 EA 45 B4 DB 0D 0A
```

该请求的 `seq` 为 `0x1234`。合法请求只触发一次 DUMP，不发送 ACK，OV56RGB5 前不输出任何文本。

## 错误处理

1. CRC、version、type、length 和 EOF 错误产生 `BINARY_ERROR`。
2. 解析错误只增加总计数和对应分类计数，不触发 DUMP。
3. 半帧达到 100 ms 产生 `BINARY_TIMEOUT`，只增加超时计数。
4. 错误或超时均不发送 `ERROR`、NACK、日志或其他文本。
5. `A5 00` 属于普通帧头搜索失败，静默回到 IDLE，不计为解析错误。
6. `A5 A5 5A` 从第二个 `A5` 快速重同步。
7. 错误字节为 `0xA5` 时保留为下一候选帧的 SOF0。
8. UART DMA 恢复成功或 StreamBuffer 溢出重同步时，同时复位文本解析器和分发器。
9. version、type、length 或 EOF0 提前报错且底层 inactive 时，只产生一次 `BINARY_ERROR`，随后分别隔离 11 B、10 B、6 B 或 1 B 固定帧尾部。
10. EOF1 和 CRC 错误发生在完整 14 B 帧末尾，剩余字节数为 0，不进入隔离。
11. 尾部隔离期间所有字节，包括 LF 和 `0xA5`，均只被丢弃，不产生 `TEXT_BYTE`、新错误或图像请求。
12. 隔离状态从建立或最后一次丢弃字节起满 100 ms 时静默解除，返回 `NONE`，不重复增加错误计数或超时计数。
13. Reset 会清除解析器、隔离活动标志、剩余计数和隔离时间戳。

## seq 与 frame_id

1. 二进制请求 `seq` 只标识 PC 发出的请求。
2. OV56RGB5 `frame_id` 只标识 STM32 成功发送的图像帧。
3. 两个字段相互独立，不进行映射。
4. `seq = 0x1234` 的请求不要求返回 `frame_id = 0x1234`。
5. 现有 Python 工具只读取 OV56RGB5 `frame_id`，本轮未修改。

## 运行统计

```text
binary_request_count
binary_request_success_count
binary_request_error_count
binary_request_crc_error_count
binary_request_version_error_count
binary_request_type_error_count
binary_request_length_error_count
binary_request_eof_error_count
binary_request_timeout_count
last_binary_request_seq
last_binary_error_code
```

合法请求但 DUMP 失败时，`binary_request_count` 增加，`binary_request_success_count` 不增加，失败仍由既有 `dump_error_count` 和 `last_error_code` 记录。

## 主机测试

主机编译器为 Visual Studio C11，编译选项为：

```text
/std:c11 /W4 /WX /utf-8
```

| 测试 | 总数 | 通过 | 失败 | 结果 |
|---|---:|---:|---:|---|
| protocol_crc32 | 既有测试 | 全部 | 0 | 通过，`123456789` 为 `0xCBF43926` |
| image_request_protocol | 47 | 47 | 0 | 通过，固定帧 CRC32 为 `0xDBB445EA` |
| camera_uart_dispatcher | 85 | 85 | 0 | 通过 |

原 62 项分发器测试全部保留，本轮新增 23 项，覆盖完整 version/type/length/EOF0/CRC 错误帧尾部静默、错误后文本与二进制恢复、隔离 99/100 ms 边界、无重复错误/超时事件和 Reset 安全。

## 固件构建

`cmake --build build/Debug` 构建通过。

| 存储区 | 使用量 | 总容量 | 占用率 |
|---|---:|---:|---:|
| RAM | 116488 B | 192 KB | 59.25% |
| CCMRAM | 0 B | 64 KB | 0.00% |
| FLASH | 66128 B | 1 MB | 6.31% |

## 尚未完成

1. 尚未新增正式 Python 二进制请求工具。
2. 尚未执行 Round 5 连续请求压力测试。
3. Round 4 二进制请求尚未完成 COM4 板上测试。
4. UART TX 仍为阻塞发送。
5. DUMP 期间 CameraServiceTask 不处理新命令，RX DMA 继续接收，持续输入可能使 StreamBuffer 溢出。
6. 正式 Python 工具和压力测试属于 Round 5，不在本轮实现。

## 已知协议边界

1. 分发层仅以 LF 结束 TEXT；裸 CR 后仍保持 TEXT，正常 `\n` 和 `\r\n` 不受影响。
2. 二进制候选超时由 `FeedByte()` 发现时，当前非 `0xA5` 字节会按底层既有语义被消费，不回灌到文本路径。
3. 尾部隔离以 v1 固定 14 B 请求帧为前提；错误帧被截断后，发送端若在 100 ms 内立即发送新消息，新消息前部会被当作旧帧尾部丢弃。
4. v1 要求 `payload_len = 0`；length 错误固定丢弃后续 6 B，非合规发送端额外发送的载荷不属于当前协议保障范围。

## 板上测试计划

串口统一使用 COM4，关闭其他占用串口的程序后运行临时验证命令：

```powershell
python -c "import serial,struct,time,zlib; s=serial.Serial('COM4',115200,timeout=8); seq=0x1234; body=struct.pack('<BBHH',1,0x20,seq,0); crc=zlib.crc32(body)&0xffffffff; req=b'\xA5\x5A'+body+struct.pack('<I',crc)+b'\x0D\x0A'; print('request=',req.hex(' ')); s.reset_input_buffer(); s.write(req); data=s.read(38426); print('received=',len(data)); print('magic=',data[:8]); s.close()"
```

预期基本结果：

```text
request= a5 5a 01 20 34 12 00 00 ea 45 b4 db 0d 0a
received=38426
magic=b'OV56RGB5'
```

还需逐项测试：

| 测试项 | 结果 |
|---|---|
| 文本 HELP | 待测试 |
| 文本 DUMP | 待测试 |
| 二进制请求后文本 STATUS | 待测试 |
| CRC 错误请求无文本响应 | 待测试 |
| 完整 version/type/length/EOF0 错误帧无文本响应 | 待测试 |
| 截断错误帧隔离满 100 ms 后恢复 | 待测试 |
| 错误请求后合法请求恢复 | 待测试 |
| seq 为 0x0000、0x1234、0xFFFF | 待测试 |
| STATUS 二进制统计 | 待测试 |

上述一行命令只验证基本接入，不能替代 Round 5 正式 Python 工具、CRC 校验和连续压力测试。

## 阶段结论

Round 4 编码、85 项分发器主机测试、既有回归测试和固件构建已通过；提前错误帧尾部已在 BINARY 内部静默隔离，OV56RGB5、UART DMA 底层和现有 Python 工具保持不变。当前仍需完成 COM4 板上错误帧静默与恢复测试后再确认硬件链路验收。
