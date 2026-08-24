# Python 连续二进制图像请求学习笔记

## 1. Round 5B 做了什么

Round 5A 只发送一次二进制图像请求，用来确认 14 B 请求帧能够触发一次 OV56RGB5 图像响应。

Round 5B 在相同协议基础上把请求过程重复 20 次。每次请求都使用不同的 seq，独立接收并校验一帧图像，最后统计成功、失败和耗时。它的目标是验证连续通信稳定性，不是给 STM32 增加新功能。

本轮不修改 STM32 固件，不修改 OV56RGB5，也不做错误注入。

## 2. 为什么要连续测试

一次成功只能说明某一次通信正确，不能证明连续工作稳定。重复请求可以帮助发现只在后续轮次出现的问题，例如：

- 第一次成功，第二次请求失败。
- DMA 接收状态没有正确进入下一轮。
- StreamBuffer 中存在上一轮残留数据。
- frame_id 更新异常。
- 某一帧 payload CRC 错误。
- 某一次接收超时。
- 一次失败之后，后续请求无法恢复。

20 次仍不是长期老化测试，但比单次测试更容易发现连续状态管理问题。

## 3. 整体流程

脚本的核心过程如下：

```text
for 循环 20 次
    ↓
构造不同 seq 的请求
    ↓
清空旧的串口输入数据
    ↓
发送 14 B 请求
    ↓
接收 38426 B 响应
    ↓
校验 header 和 payload CRC
    ↓
记录成功、失败和耗时
    ↓
等待 0.2 秒
```

单次校验失败时，循环不会立即结束，而是继续发送下一次请求。

## 4. for 循环怎么理解

连续测试使用：

```python
for index in range(REQUEST_COUNT):
```

当 `REQUEST_COUNT=20` 时，`range(20)` 依次产生 0 到 19。每次循环把其中一个数字放入 `index`。

因此：

```text
第 1 次循环：index = 0
第 2 次循环：index = 1
...
第 20 次循环：index = 19
```

请求序号使用：

```python
seq = (START_SEQ + index) & 0xFFFF
```

当 `START_SEQ=1` 时，20 次请求的 seq 依次为 1 到 20。`& 0xFFFF` 把结果限制在 16 位范围内；超过 `0xFFFF` 后会从 0 重新开始。

## 5. 为什么每次 seq 不同

seq 用于区分 PC 发出的请求。每次递增 seq，可以在发送端看出当前是第几条请求，也为以后更复杂的协议关联留下基础。

需要注意：请求 seq 与响应 frame_id 相互独立。

- seq 由 PC 脚本生成。
- frame_id 由 STM32 管理。
- 脚本不要求 seq 等于 frame_id。
- 不能用 seq 覆盖 OV56RGB5 中原有的 frame_id。

## 6. 如何测量耗时

每次请求发送前记录开始时间，接收完成后记录结束时间：

```python
start_time = time.monotonic()
```

```python
end_time = time.monotonic()
elapsed_ms = (end_time - start_time) * 1000.0
```

两个时间相减得到秒数，再乘 1000 转换成毫秒。

这里使用 `time.monotonic()`，因为它是只向前增加的单调时钟，不会因为用户修改系统时间或网络校时而突然跳变，适合测量一段程序执行了多久。

## 7. 列表如何保存耗时

脚本先创建一个空列表：

```python
elapsed_times = []
```

一帧校验成功后，把本次耗时追加到列表末尾：

```python
elapsed_times.append(elapsed_ms)
```

例如三次成功耗时分别为 3500、3480、3520 毫秒，列表会变成：

```text
[3500, 3480, 3520]
```

只有成功帧的耗时进入列表，失败帧仍会在单次输出中显示实际等待时间，但不参与成功帧平均耗时计算。

## 8. 如何计算平均值

当列表中至少有一项时，平均耗时使用：

```python
average_ms = sum(elapsed_times) / len(elapsed_times)
```

- `sum(elapsed_times)` 把列表中的所有耗时相加。
- `len(elapsed_times)` 得到成功帧数量。
- 总耗时除以数量就是平均值。

如果一次成功请求都没有，列表为空，不能除以 0。脚本会直接打印“平均耗时：无”。

## 9. min 和 max 的作用

`min()` 返回列表中的最小值，用于表示最快的一次成功请求：

```python
minimum_ms = min(elapsed_times)
```

`max()` 返回列表中的最大值，用于表示最慢的一次成功请求：

```python
maximum_ms = max(elapsed_times)
```

平均、最短和最长耗时放在一起，可以初步观察每次图像传输的时间是否稳定。

## 10. 如何计算成功率

成功率的计算公式是：

```python
success_rate = success_count * 100.0 / REQUEST_COUNT
```

例如 20 次请求成功 18 次：

```text
18 × 100 ÷ 20 = 90%
```

使用 `100.0` 会得到带小数的结果，脚本用 `:.2f` 显示两位小数，例如 `90.00%`。

## 11. 为什么单次失败后继续测试

如果第 5 次请求失败，仍然需要观察第 6 到第 20 次是否能够成功。

这样可以区分两种情况：

1. 只有一次偶发超时或数据错误，后续通信能够恢复。
2. 一次失败后接收链路永久卡住，剩余请求全部失败。

因此协议校验失败只增加 `failure_count` 并打印原因，不会在循环中直接 `return`。无法打开串口等全局错误则会结束程序，因为此时无法继续发送后续请求。

## 12. frame_id 连续性检查

STM32 只有在完整发送一帧图像后才更新 frame_id。脚本保存每个校验成功响应的 frame_id，然后检查相邻值：

```python
for index in range(1, len(received_frame_ids)):
    previous_id = received_frame_ids[index - 1]
    current_id = received_frame_ids[index]

    if current_id != ((previous_id + 1) & 0xFFFFFFFF):
        frame_ids_continuous = False
        break
```

这里的规则是当前 frame_id 应等于上一个 frame_id 加 1，并考虑 32 位回绕。

需要正确理解：

- frame_id 由 STM32 管理。
- 请求 seq 与 frame_id 独立。
- 脚本只观察成功响应中的 frame_id 是否连续。
- 不要求第一个 frame_id 从 1 开始，因为开发板可能此前已经发送过图像。
- frame_id 不连续会打印警告，但不会单独让整个测试失败。

## 13. PASS 代表什么

Round 5B 显示 `PASS` 表示：

1. 20 次请求都收到了 38426 B 响应。
2. 20 次响应的 magic、version 和 pixel_format 正确。
3. 20 次响应的图像尺寸都是 160x120。
4. 20 次响应的 payload_len 和实际 payload 长度都是 38400 B。
5. 20 次 payload CRC32 全部正确。
6. 没有请求超时或检测到的数据损坏。

最终条件是：

```python
success_count == REQUEST_COUNT and failure_count == 0
```

满足时程序退出码为 0，否则显示 `FAIL` 并返回非 0。

## 14. PASS 不能证明什么

20 次全部成功仍不能证明所有情况都没有问题：

- 20 次不是数小时或数天的长期老化测试。
- 没有测试错误 version、type、length、CRC 或 EOF。
- 没有主动制造 StreamBuffer 溢出。
- 没有主动制造 UART 硬件错误并验证恢复。
- 没有测试多个 PC 程序同时访问串口。
- UART TX 当前仍为阻塞发送方式。

因此本轮结果只代表固定条件下的短时间连续通信验证。

## 15. 面试时怎么解释

可以这样回答：

> 单次协议测试通过后，我又使用 Python 脚本连续发送 20 次图像请求。每次请求使用不同 seq，收到响应后检查 OV56RGB5 帧头、长度和 payload CRC，并统计成功率、平均耗时和最大耗时。单次失败不会立即停止，从而可以观察协议是否能够在异常后继续运行。该脚本用于验证 STM32 端 UART DMA、StreamBuffer 和协议状态机的连续通信稳定性。

这段回答重点说明连续请求、逐帧校验、统计指标和恢复观察，不需要把 Python 工具描述成复杂测试框架。

## 16. 本轮新增的 Python 知识

- `for`：重复执行一段代码。
- `range(20)`：依次产生 0 到 19。
- `list`：按顺序保存多个值。
- `append(value)`：把一个新值追加到列表末尾。
- `len(list)`：获取列表包含多少项。
- `sum(list)`：计算列表所有数值之和。
- `min(list)`：找出最小值。
- `max(list)`：找出最大值。
- 百分比计算：成功数乘 100，再除以总数。
- `break`：满足条件时提前结束当前循环。

这些语法足以理解脚本的连续请求和统计部分，不需要类、线程、asyncio 或复杂第三方库。

## 17. 下一步

Round 5C 计划进行错误恢复验证：

- 构造错误 version。
- 构造错误 type。
- 构造错误 length。
- 构造错误 CRC。
- 构造错误 EOF。
- 确认 STM32 对错误二进制请求保持静默。
- 确认错误请求之后，下一条合法请求仍能正常得到 OV56RGB5 响应。

Round 5C 开始前，应先关闭 MobaXterm、确认 COM4 空闲，并完成本轮 20 次连续请求的实际板上测试。
