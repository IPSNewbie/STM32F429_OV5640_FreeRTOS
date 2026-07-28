# Python 串口二进制图像请求学习笔记

## 1. 这段脚本解决什么问题

MobaXterm 串口终端适合输入 `HELP`、`STATUS`、`DUMP` 等文本命令，但二进制请求中包含 `0xA5`、`0x00` 等不能直接从键盘可靠输入的字节。

`tools/uart_image_request_basic.py` 使用 Python 完成一次最小协议验证：

1. 打开 COM4。
2. 动态构造 14 B `REQUEST_IMAGE` 请求。
3. 向 STM32 发送请求。
4. 循环接收固定长度的 38426 B OV56RGB5 图像帧。
5. 解析 22 B header。
6. 提取 38400 B RGB565 payload。
7. 重新计算 payload CRC32，并输出 `PASS` 或 `FAIL`。

STM32 端的 UART DMA、StreamBuffer、协议状态机和 FreeRTOS 任务处理仍是项目核心。Python 只是在 PC 端构造请求和验证响应，不改变固件协议。

## 2. 运行前准备

1. 给开发板刷入已完成 Stage 9 Round 4 的固件。
2. 关闭 MobaXterm，确保 COM4 没有被其他程序占用。
3. 确认串口参数为 115200、8 数据位、1 停止位、无校验、无流控。
4. 安装 pyserial：

   ```powershell
   python -m pip install pyserial
   ```

5. 在工程根目录运行：

   ```powershell
   python tools/uart_image_request_basic.py
   ```

脚本不会发送文本 `DUMP` 或 `STATUS`，也不会保存 PNG。它只验证一次二进制请求和一次图像响应。

## 3. 五个 import 分别做什么

脚本只导入五个模块：

- `serial`：由 pyserial 提供，用于打开 COM4、发送请求和接收响应。
- `struct`：按照指定字节序打包整数，或从字节中解析整数。
- `zlib`：计算与 STM32 协议一致的 CRC32。
- `time`：使用单调时钟实现总接收超时，并在打开串口后短暂等待。
- `sys`：把 `main()` 的返回值交给操作系统，形成明确的退出码。

其中 `struct`、`zlib`、`time`、`sys` 属于 Python 标准库；只有 `serial` 需要额外安装 pyserial。

## 4. 顶部常量

脚本把需要调整的配置集中放在顶部：

```python
PORT = "COM4"
BAUD = 115200
TIMEOUT_SECONDS = 8.0
REQUEST_SEQ = 0x1234
```

- `PORT`：串口号，本项目统一使用 COM4。
- `BAUD`：波特率，固定为 115200。
- `TIMEOUT_SECONDS`：接收完整图像帧允许的总时间，当前为 8 秒。
- `REQUEST_SEQ`：PC 请求序号，当前为 `0x1234`。

协议长度常量如下：

```python
REQUEST_FRAME_SIZE = 14
IMAGE_HEADER_SIZE = 22
IMAGE_PAYLOAD_SIZE = 38400
IMAGE_CRC_SIZE = 4
IMAGE_FRAME_SIZE = 38426
```

完整响应长度的关系是：

```text
22 B header + 38400 B payload + 4 B CRC32 = 38426 B
```

## 5. `struct.pack("<BBHH")` 怎么理解

请求中 CRC 覆盖的 6 B 内容由下面代码生成：

```python
body = struct.pack("<BBHH", 0x01, 0x20, seq, 0)
```

格式字符串 `"<BBHH"` 可以拆开理解：

- `<`：后续多字节整数使用小端序，低字节先发送。
- 第一个 `B`：1 B 无符号整数，对应 `version=0x01`。
- 第二个 `B`：1 B 无符号整数，对应 `msg_type=0x20`。
- 第一个 `H`：2 B 无符号整数，对应 `seq`。
- 第二个 `H`：2 B 无符号整数，对应 `payload_len=0`。

当 `seq=0x1234` 时，打包后的 body 为：

```text
01 20 34 12 00 00
```

`0x1234` 在小端序中写成 `34 12`，这就是低字节先出现的含义。

## 6. CRC32 怎么计算

请求 CRC 的计算代码是：

```python
crc = zlib.crc32(body) & 0xFFFFFFFF
```

`zlib.crc32(body)` 对 body 的 6 B 数据计算 CRC32。`& 0xFFFFFFFF` 只保留低 32 位，使结果明确落在 `0x00000000` 到 `0xFFFFFFFF` 范围内，也便于不同 Python 版本和其他语言统一表示。

当 `seq=0x1234` 时：

```text
CRC32 数值：0xDBB445EA
小端字节：EA 45 B4 DB
```

CRC 数值使用 `struct.pack("<I", crc)` 转为 4 B 小端数据。`I` 表示 4 B 无符号整数。

## 7. 14 B 请求帧如何组成

完整请求由四段拼接：

```python
request = b"\xA5\x5A" + body + struct.pack("<I", crc) + b"\x0D\x0A"
```

各段含义如下：

| 段 | 长度 | 内容 |
|---|---:|---|
| SOF | 2 B | 固定帧头 `A5 5A` |
| body | 6 B | version、msg_type、seq、payload_len |
| CRC32 | 4 B | 只覆盖 body，按小端序发送 |
| EOF | 2 B | 固定帧尾 `0D 0A` |

当 `seq=0x1234` 时，动态计算出的完整请求必须是：

```text
A5 5A 01 20 34 12 00 00 EA 45 B4 DB 0D 0A
```

脚本没有把这 14 B 直接硬编码为实际发送数据，而是每次由 `struct.pack()` 和 `zlib.crc32()` 计算生成。

## 8. 为什么不能只调用一次 `ser.read(38426)`

串口数据会分批到达。USB 转串口芯片、操作系统缓冲区和 Python 调度都可能使一次 `ser.read()` 只返回部分数据，即使 STM32 最终会发满 38426 B。

`read_exact()` 因此使用循环：

```python
data = bytearray()
deadline = time.monotonic() + timeout_seconds
while len(data) < expected_size and time.monotonic() < deadline:
    chunk = ser.read(expected_size - len(data))
    if chunk:
        data.extend(chunk)
```

- `bytearray()` 创建可追加的字节容器。
- `deadline` 是允许接收的最晚时间。
- 每次只请求尚未收到的字节数。
- `extend()` 把新收到的一批字节追加到末尾。
- 收满目标长度或超过总超时后退出，不会无限等待。

如果超时，函数仍返回已经收到的数据，`main()` 会报告实际长度，例如 `只收到 20000/38426 B`。

## 9. 如何解析 22 B header

OV56RGB5 header 字段如下：

| 偏移 | 字段 | 长度 | 解析方法 |
|---:|---|---:|---|
| 0 | magic | 8 B | 字节切片 |
| 8 | version | 1 B | 单字节索引 |
| 9 | pixel_format | 1 B | 单字节索引 |
| 10 | width | 2 B | 小端 `H` |
| 12 | height | 2 B | 小端 `H` |
| 14 | payload_len | 4 B | 小端 `I` |
| 18 | frame_id | 4 B | 小端 `I` |

对应代码为：

```python
magic = response[0:8]
version = response[8]
pixel_format = response[9]
width, height = struct.unpack("<HH", response[10:14])
payload_len, frame_id = struct.unpack("<II", response[14:22])
```

`response[0:8]` 表示从偏移 0 取到偏移 8 之前，共 8 B。`struct.unpack()` 与打包相反，把字节恢复成 Python 整数。

脚本检查 magic 为 `b"OV56RGB5"`、version 为 1、pixel_format 为 1、尺寸为 160x120、payload_len 为 38400。

## 10. payload CRC 如何校验

STM32 发送 22 B header 后，再发送 38400 B payload，最后附加 4 B CRC32。CRC 只覆盖 payload，不覆盖 header。

Python 先提取 payload：

```python
payload = response[22:22 + payload_len]
```

再解析 STM32 附加的 CRC，并重新计算本地 CRC：

```python
received_crc = struct.unpack("<I", response[-4:])[0]
calculated_crc = zlib.crc32(payload) & 0xFFFFFFFF
```

两者相等说明本次串口传输中的 RGB565 图像数据通过完整性校验；两者不相等时脚本输出 `FAIL`，且退出码非 0。

## 11. seq 与 frame_id 为什么不相等

- `seq` 标识 PC 发出的二进制请求。
- `frame_id` 标识 STM32 成功发送的 OV56RGB5 图像帧。
- 当前协议中二者相互独立。

因此请求 `seq=0x1234` 时，响应中的 `frame_id` 可以是 1、2 或其他由固件当前状态决定的值。不能用请求 seq 覆盖旧协议的 frame_id，也不应把二者不相等判断为错误。

## 12. 面试时怎么解释

可以简洁地回答：

> 我使用 Python 的 pyserial 编写了 PC 端测试脚本。脚本按照自定义协议动态构造 14 字节图像请求，使用 struct 处理小端字段，使用 zlib 计算 CRC32。STM32 收到请求后返回 OV56RGB5 图像帧，脚本循环接收固定长度数据，解析尺寸、帧号和 payload 长度，并重新计算 payload CRC。Python 只用于协议验证，核心嵌入式实现是 UART DMA、StreamBuffer、协议状态机和 FreeRTOS 任务处理。

这段说明重点体现了协议构造、字节序、可靠接收和 CRC 完整性校验，不需要夸大 Python 工具的作用。

## 13. 最少需要会写的 Python 代码

打开串口：

```python
ser = serial.Serial("COM4", 115200, timeout=0.2)
```

发送和接收字节：

```python
ser.write(data)
data = ser.read(size)
```

打包一个 2 B 小端整数：

```python
data = struct.pack("<H", value)
```

解析一个 4 B 小端整数：

```python
value = struct.unpack("<I", data)[0]
```

计算 CRC32：

```python
crc = zlib.crc32(data) & 0xFFFFFFFF
```

还需要理解以下基础语法：

- `for` 或 `while`：重复接收数据。
- `if`：检查协议字段和 CRC 是否正确。
- `try/except`：捕获串口、超时或数据格式错误，并输出清楚的失败原因。
- `return 0` 与 `return 1`：分别表示成功和失败。

## 14. 本轮暂未实现

Stage 9 Round 5A 刻意保持最小范围，尚未实现：

- 连续请求或压力测试。
- 成功率、失败率和耗时统计。
- CRC、版本、长度等错误注入。
- PNG 保存和图像显示。
- GUI。
- 命令行参数。
- 线程或 asyncio。

这些功能不影响本轮验证“二进制请求能够触发一次 OV56RGB5 响应”。

## 15. 下一步

Round 5B 计划在不修改 STM32 固件的前提下：

1. 连续请求 20 次。
2. 统计成功次数、失败次数和成功率。
3. 统计平均耗时与最大耗时。
4. 继续逐帧检查长度、magic、尺寸和 payload CRC。

在开始 Round 5B 前，应先逐行读懂本轮脚本，并在关闭 MobaXterm、确认 COM4 空闲后完成一次 Round 5A 板上测试。
