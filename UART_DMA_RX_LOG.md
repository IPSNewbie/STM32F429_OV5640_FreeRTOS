# UART DMA 接收链路日志

## 阶段

Stage 9 Round 3：UART DMA + IDLE + StreamBuffer 与文本 CLI 迁移

## 当前基础

1. Stage 9 Round 1 已提取公共 CRC32 模块。
2. Stage 9 Round 2 已新增纯 C 二进制图像请求解析器，并通过 47 项主机测试。
3. 当前文本 CLI 支持 HELP、STATUS、PROC、THR、RESET 和 DUMP。
4. CameraServiceTask 是 UART、CLI 和 DUMP 的唯一业务消费者。
5. MonitorTask 只维护心跳和运行时间，不访问 UART。
6. 文本 `DUMP` 返回既有 OV56RGB5 图像帧。
7. 本轮只迁移文本 CLI 和文本 `DUMP` 的接收链路，不接入二进制图像请求解析器。

## CubeMX 配置

以下配置来自当前 CubeMX 生成代码和工程配置，不使用提示词推测值。

| 项目 | 实际配置 |
|---|---|
| USART 实例 | USART1 |
| DMA Controller | DMA2 |
| DMA Stream | DMA2 Stream2 |
| DMA Channel | Channel 4 |
| DMA 方向 | Peripheral to Memory |
| DMA 数据宽度 | Peripheral Byte / Memory Byte |
| DMA 内存递增 | 启用 |
| DMA 模式 | Circular |
| DMA 优先级 | Medium |
| USART1 IRQ | 抢占优先级 5，子优先级 0，已启用 |
| DMA2 Stream2 IRQ | 抢占优先级 5，子优先级 0，已启用 |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 |

USART1 和 DMA2 Stream2 的中断优先级数值均为 5，处于允许调用 FreeRTOS `FromISR` API 的边界。不得将它们提高到数值 0～4 后继续调用 StreamBuffer 的 ISR API。

## 模块结构

本轮新增 `uart_rx_dma` 模块：

```text
USART1 RX
    ->
DMA2 Stream2 Channel 4，Circular
    ->
HAL_UARTEx_RxEventCallback
    ->
DMA 位置差分
    ->
xStreamBufferSendFromISR
    ->
静态 StreamBuffer
    ->
xStreamBufferReceive
    ->
CameraServiceTask
    ->
camera_pc_dump 文本行解析
    ->
camera_cli 或文本 DUMP
```

模块公开接口包括初始化、任务侧读取、RX 事件处理、错误记录、任务侧错误恢复、溢出查询和清除、缓冲排空及统计读取。

## DMA 位置差分算法

DMA 缓冲区大小为 128 B，模块维护上一次已处理位置 `old_position`。

1. `size > old_position`：搬运 `[old_position, size)`。
2. `size < old_position`：先搬运 `[old_position, 128)`，再搬运 `[0, size)`。
3. `size == old_position`：没有新增数据，不重复搬运。
4. `size == 128`：保留 128 这个边界位置，过滤 TC 后紧接的同位置 IDLE 重复事件。
5. 处理完成后更新 `old_position`。
6. Circular 模式下不在 IDLE、HT 或 TC 回调中重新启动 DMA。

## StreamBuffer

1. StreamBuffer 有效容量为 512 B。
2. 触发级别为 1 B。
3. 使用 `xStreamBufferCreateStatic()` 静态创建。
4. 静态存储区额外保留一个 FreeRTOS 环形缓冲空槽。
5. 不调用动态版本 `xStreamBufferCreate()`。
6. ISR 使用 `xStreamBufferSendFromISR()`。
7. CameraServiceTask 使用 `xStreamBufferReceive()`。
8. StreamBuffer 只有一个 ISR 写入端和一个 CameraServiceTask 读取端。

## ISR 职责边界

RX 事件回调只执行：

1. 检查是否为已保存的目标 UART 句柄。
2. 增加 RX 事件计数。
3. 计算本次新增 DMA 区间。
4. 将新增字节写入 StreamBuffer。
5. 记录新增字节、实际写入字节和溢出字节。
6. 使用 `portYIELD_FROM_ISR()` 按需唤醒任务。

UART 错误回调只执行：

1. 增加 UART DMA 错误计数。
2. 设置任务侧恢复标志。

ISR 不执行 printf、UART TX、CLI、DUMP、图像采集、协议解析、缓冲清空或复杂 DMA 重启。

## CameraServiceTask 流程

1. 任务启动后检查 UART 句柄。
2. 调用一次 `UART_RxDma_Init()`，正常运行期间不重复启动 RX DMA。
3. 每轮先检查 UART DMA 恢复请求和 StreamBuffer 溢出标志。
4. 每次最多读取 32 B，最长阻塞等待 100 ms。
5. 读取超时时增加兼容字段 `uart_none_count`。
6. 对读取块中的每个字节继续执行现有文本行解析。
7. 一个读取块包含多条命令时按顺序处理全部命令。
8. 完整 CLI 命令仍由 `Camera_CLI_HandleLine()` 处理。
9. 完整 `DUMP` 仍在 CameraServiceTask 中串行执行。
10. DUMP 期间 DMA 继续接收，但 CameraServiceTask 暂停解析新命令。
11. DUMP 结束后继续处理本地读取块和 StreamBuffer 中的数据；如果已经溢出，则放弃残缺输入并重同步。

## 文本 CLI 迁移

文本行状态机继续位于 `camera_pc_dump.c`，只把 UART 单字节读取与文本行组装分离。

1. `Camera_PC_Dump_FeedCommandByte()` 接收一个已由 StreamBuffer 取出的字节。
2. `Camera_PC_Dump_ResetCommandParser()` 无输出地清除残缺文本行。
3. 兼容 LF 和 CRLF。
4. 空行和连续换行不会产生未知命令。
5. 超长行继续丢弃至下一个换行，再恢复正常解析。
6. 文本 `DUMP` 仍在上游识别，不进入普通 CLI 未知命令路径。
7. 旧 `HAL_UART_Receive()` 单字节轮询已经停用。
8. 不再依赖 1 ms UART 轮询超时，也不再使用 PENDING 后忙轮询。

## 溢出恢复

1. ISR 实际写入少于请求长度时，差值计入 `stream_overflow_bytes` 并设置溢出标志。
2. ISR 不清空 StreamBuffer，也不执行文本重同步。
3. CameraServiceTask 检测到溢出后清除当前文本行状态。
4. 当前读取块尚未处理的字节直接放弃。
5. CameraServiceTask 非阻塞排空 StreamBuffer 中的现有字节。
6. 只有确认 StreamBuffer 已排空时才清除溢出标志。
7. 如果清除过程中又有字节到达，则保留标志并在下一轮继续排空。
8. 成功重同步后增加 `stream_buffer_resync_count`。
9. 恢复过程不输出文本，避免污染图像二进制响应。

## UART 错误恢复

1. UART 错误 ISR 只记录错误并设置恢复请求。
2. CameraServiceTask 上下文调用 `HAL_UART_AbortReceive()`。
3. 清除 PE、FE、NE 和 ORE 错误标志。
4. 将 DMA 旧位置重置为 0。
5. 重新调用一次 `HAL_UARTEx_ReceiveToIdle_DMA()`。
6. 恢复成功后增加 `uart_dma_recovery_count` 并清除残缺文本状态。
7. 恢复失败时保留恢复请求，后续继续重试，不解析旧数据。
8. UART TX 路径不变。

## 统计字段

Stage 8 原有统计字段继续保留：

```text
uart_none_count
uart_pending_count
uart_error_count
```

Round 3 中含义调整为：

1. `uart_none_count`：StreamBuffer 阻塞读取超时次数。
2. `uart_pending_count`：文本字节已消费但当前行尚未完整的次数。
3. `uart_error_count`：同步 UART DMA 错误计数的兼容字段。

STATUS 新增：

```text
uart_dma_event_count
uart_dma_rx_bytes
stream_buffer_write_bytes
stream_buffer_overflow_bytes
uart_dma_error_count
uart_dma_recovery_count
stream_buffer_resync_count
```

## 构建结果

1. `cmake --build build/Debug`：通过。
2. RAM：116408 B / 192 KB，59.21%。
3. CCMRAM：0 B / 64 KB，0.00%。
4. FLASH：62736 B / 1 MB，5.98%。
5. CRC32 主机测试：通过，`123456789` 为 `0xCBF43926`，空数据为 `0x00000000`，退出码 0。
6. 二进制请求解析器主机测试：总数 47，通过 47，失败 0，退出码 0。
7. `git diff --check`：通过，仅有工作区 LF/CRLF 换行提示，无空白错误。

## COM4 测试方法

MobaXterm：

```text
Serial port = COM4
Speed = 115200
Data bits = 8
Stop bits = 1
Parity = None
Flow control = None
Serial engine = PuTTY
```

### 基本命令

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

### 快速连续命令

快速连续发送：

```text
HELP
STATUS
HELP
STATUS
```

### 分段命令

分多次发送 `HE`、`LP\r\n`，以及 `PROC GR`、`AY\r\n`，确认能够组成完整命令。

### 超长和未知命令

发送超长字符串和 `UNKNOWN_TEST`，随后发送 `HELP`，确认错误后仍能恢复。

### 文本 DUMP

关闭 MobaXterm 后运行：

```powershell
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r3_bypass
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r3_gray
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r3_binary
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag stage9_r3_reset
```

板上 DUMP 验收时需要同时确认：

1. 所有图像帧 CRC 校验通过。
2. OV56RGB5 magic、header、payload 和 CRC 字节序保持不变。
3. 第二次及后续 DUMP 仍能正常完成。
4. `uart_dma_error_count` 保持为 0。
5. `stream_buffer_overflow_bytes` 保持为 0。

### 连续性

1. 连续发送 HELP 20 次。
2. 连续发送 STATUS 20 次。
3. 确认每条命令均有独立响应。

## 测试结果

| 测试项 | 结果 | 说明 |
|---|---|---|
| HELP | 待测试 | COM4 板上测试 |
| STATUS | 待测试 | 检查新增 DMA 统计 |
| 快速连续命令 | 待测试 | 检查粘连和丢失 |
| 分段命令 | 待测试 | 检查跨 DMA 事件组行 |
| 超长和未知命令恢复 | 待测试 | 随后 HELP 应正常 |
| BYPASS DUMP | 待测试 | 检查 OV56RGB5 和 CRC |
| GRAY DUMP | 待测试 | 检查模式保持 |
| BINARY DUMP | 待测试 | 阈值 128 |
| RESET DUMP | 待测试 | 恢复 BYPASS |
| HELP 连续 20 次 | 待测试 | 每次均应响应 |
| STATUS 连续 20 次 | 待测试 | 每次均应响应 |

## 风险与限制

1. UART TX 仍为阻塞发送。
2. 单次 DUMP 约 3.5 秒。
3. DUMP 期间 CameraServiceTask 不解析新命令。
4. StreamBuffer 有效容量只有 512 B，DUMP 期间持续输入可能造成溢出和整段丢弃。
5. 二进制请求协议尚未接入。
6. 需要重点测试快速连续命令、跨 DMA 事件的分段命令和 DUMP 期间输入。
7. UART 和 DMA IRQ 优先级处于 FreeRTOS 系统调用边界，后续修改 NVIC 时必须重新核对。
8. 本轮没有执行 COM4 硬件测试，不能据此声明板上链路已经通过。

## 阶段结论

代码构建已通过，COM4 板上测试待进行。
