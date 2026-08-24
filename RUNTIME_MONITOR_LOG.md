# 运行监控与状态统计日志

## 阶段

Stage 8：运行监控与状态统计

## 当前基础

1. Stage 7 FreeRTOS 最小任务化已完成。
2. 工程使用 CMSIS-RTOS v2。
3. CubeMX 任务 `CameraServiceTa` 通过 `StartCameraServiceTask()` 进入摄像头服务业务。
4. CameraServiceTask 独占 UART、CLI 和 DUMP。
5. MonitorTask 只维护心跳和累计运行时间。
6. `main.c` 不再运行旧裸机 UART / DUMP 命令循环。
7. 串口及测试命令统一使用 COM4。
8. OV56RGB5 图像协议和 Python 工具保持不变。

## 工程规范

1. 新增或修改代码时，新增注释统一使用中文。
2. 已有英文注释暂不批量翻译，避免引入无关差异。
3. 本阶段不新增 Queue、Semaphore、Mutex、StreamBuffer、Event Flags 或 Software Timer。
4. 本阶段不新增 UART DMA + IDLE。
5. 本阶段不拆分 CameraCaptureTask、ImageProcessTask 或 UartTxTask。

## 本阶段目标

1. 增加 FreeRTOS 运行统计。
2. 增强 STATUS 文本输出。
3. 记录任务循环、心跳和累计运行时间。
4. 记录 CLI 命令次数和未知命令次数。
5. 记录 DUMP 请求、成功、失败和单次耗时。
6. 记录 UART NONE、PENDING、ERROR 状态。
7. 记录最近错误码和 STATUS 执行时间。
8. 不改变图像采集、处理、双缓冲和 OV56RGB5 数据流。

## 任务职责

### CameraServiceTask

负责：

- UART 单字节轮询接收。
- CLI 文本命令处理及 STATUS 输出。
- DUMP 图像采集、图像处理和 OV56RGB5 发送。
- CameraServiceTask、CLI、DUMP 和 UART 状态统计。

CameraServiceTask 是运行阶段唯一访问 UART 的任务。DUMP 开始后不会插入 STATUS 或其他文本输出。

### MonitorTask

每 1000 ms 执行：

```text
monitor_tick_count++
uptime_ms += 1000
```

MonitorTask 不使用 UART，不调用 CLI 或 DUMP，不访问 DCMI/DMA、frame buffer，也不处理图像。

## 统计字段说明

| 字段 | 写入者 | 说明 |
|---|---|---|
| `camera_service_loop_count` | CameraServiceTask | 服务主循环执行次数，每轮循环递增 |
| `monitor_tick_count` | MonitorTask | 1000 ms 周期心跳次数 |
| `cli_command_count` | CameraServiceTask 的 CLI 调用链 | 已接收的完整文本 CLI 命令次数，不包含 DUMP |
| `cli_unknown_count` | CameraServiceTask 的 CLI 调用链 | 未知 CLI 命令次数 |
| `dump_request_count` | CameraServiceTask | 收到完整 DUMP 命令的次数 |
| `dump_success_count` | CameraServiceTask | DUMP 完整成功次数 |
| `dump_error_count` | CameraServiceTask | DUMP 失败次数 |
| `uart_none_count` | CameraServiceTask | 本轮 UART 没有收到数据的次数 |
| `uart_pending_count` | CameraServiceTask | 已消费字节但命令尚未完整的次数 |
| `uart_error_count` | CameraServiceTask | `HAL_ERROR` 接收错误次数 |
| `last_error_code` | CameraServiceTask 的业务调用链 | 最近一次记录的数字错误码 |
| `last_dump_time_ms` | CameraServiceTask | 最近一次 DUMP 的耗时，单位为毫秒 |
| `last_status_time_ms` | CameraServiceTask 的 CLI 调用链 | 最近一次 STATUS 执行时的 HAL tick |
| `uptime_ms` | MonitorTask | 按 1000 ms 心跳累计的运行时间 |

所有字段均为 `volatile uint32_t`，不使用浮点数或字符串，不进行动态内存分配。当前仅有 CameraServiceTask 写入大部分字段，MonitorTask 只写心跳和累计运行时间；STATUS 允许短暂的非原子一致性，因此不新增 Mutex。

## 错误码说明

| 错误码 | 含义 |
|---:|---|
| `0` | 无错误 |
| `1` | UART 句柄为空 |
| `2` | 通用 DUMP 失败 |
| `3` | 运行状态异常或 UART 接收错误 |
| `4` | 未知 CLI 命令 |
| `0x00000100 + 子错误码` | DCMI 快照启动失败 |
| `0x00000200` | DCMI 快照超时 |
| `0x00000300 + 子错误码` | 帧缓冲提交失败 |
| `0x00000400 + 子错误码` | 图像处理失败 |
| `0x00000500 + 子错误码` | OV56RGB5 图像发送失败 |

STATUS 直接输出数字错误码，本阶段不增加错误字符串表。

## UART 状态记录规则

1. `PENDING` 表示本轮已收到字节但命令尚未完整，增加 `uart_pending_count` 后立即继续轮询，绝对不执行 `osDelay`。
2. `NONE` 表示本轮没有收到数据，增加 `uart_none_count` 后执行 `osDelay(1U)`。
3. `HAL_TIMEOUT` 返回 `NONE`，保留已经接收的行缓存。
4. 只有 `HAL_ERROR` 才清 UART 错误并重置行缓存，同时增加 `uart_error_count`。
5. 当前仍使用 `HAL_UART_Receive()` 单字节轮询，没有 UART DMA 或中断环形缓冲。

## STATUS 输出格式示例

以下内容仅表示输出格式，尖括号中的值不是实测数据：

```text
process mode: <BYPASS/GRAYSCALE/BINARY>
binary threshold: <0..255>
AEC: OV5640_AEC_TARGET_BASELINE
AWB: OV5640_AWB_MODE_AUTO
image tuning: brightness=+1 contrast=0 saturation=1 sharpness=0
frame size: 160x120
RTOS:
  camera_service_loop_count=<uint32>
  monitor_tick_count=<uint32>
  uptime_ms=<uint32>
  cli_command_count=<uint32>
  cli_unknown_count=<uint32>
  dump_request_count=<uint32>
  dump_success_count=<uint32>
  dump_error_count=<uint32>
  uart_none_count=<uint32>
  uart_pending_count=<uint32>
  uart_error_count=<uint32>
  last_error_code=<uint32>
  last_dump_time_ms=<uint32>
  last_status_time_ms=<uint32>
```

STATUS 是文本 CLI 命令，只能由 CameraServiceTask 输出。MonitorTask 不主动打印 STATUS。

## DUMP 耗时统计

CameraServiceTask 在执行 DUMP 前读取 `HAL_GetTick()`，在流程结束后使用无符号差值计算耗时：

```text
elapsed_ms = HAL_GetTick() - start_tick
```

耗时记录到 `last_dump_time_ms`。本阶段不使用浮点数，不计算复杂 FPS。

## 协议保持

DUMP 数据流保持为：

```text
DUMP
    ->
DCMI/DMA capture
    ->
back buffer
    ->
commit
    ->
BYPASS / GRAYSCALE / BINARY
    ->
commit
    ->
OV56RGB5 + 22 B header + payload + CRC32
    ->
COM4
    ->
Python
```

OV56RGB5 magic、22 B header、160x120 RGB565 payload、frame ID、CRC32 算法和发送顺序均不改变。

## 测试环境

MobaXterm Serial：

```text
Serial port = COM4
Speed = 115200
Flow control = None
Data bits = 8
Stop bits = 1
Parity = None
Serial engine = PuTTY
```

MobaXterm 和 Python 不能同时占用 COM4。

## 测试步骤

### STATUS 基础检查

MobaXterm 依次发送：

```text
HELP
STATUS
STATUS
```

检查原有处理模式和阈值仍然存在，并检查 RTOS 统计字段是否完整。

### 未知命令统计

MobaXterm 发送：

```text
UNKNOWN_TEST
STATUS
```

检查 `cli_unknown_count` 和 `last_error_code`。

### BYPASS DUMP

MobaXterm 发送 `RESET` 后关闭串口连接，运行：

```bash
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag rtos_monitor_bypass
```

### GRAYSCALE DUMP

MobaXterm 发送 `PROC GRAY` 后关闭串口连接，运行：

```bash
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag rtos_monitor_gray
```

### BINARY DUMP

MobaXterm 发送：

```text
PROC BINARY
THR 128
```

关闭串口连接后运行：

```bash
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag rtos_monitor_binary
```

每次 Python DUMP 完成后关闭 Python 串口，再使用 MobaXterm 发送 `STATUS` 检查 DUMP 请求、结果和耗时统计。

## 测试结果

| 测试项 | 方法 | 结果 | 说明 |
|---|---|---|---|
| HELP 与 STATUS 基本输出 | MobaXterm 发送 `HELP`、`STATUS` | 待测试 | |
| MonitorTask 心跳 | 间隔数秒读取两次 STATUS | 待测试 | |
| CLI 命令计数 | 连续执行已知 CLI 命令后读取 STATUS | 待测试 | |
| 未知命令计数 | 发送 `UNKNOWN_TEST` 后读取 STATUS | 待测试 | |
| DUMP BYPASS | Tag：`rtos_monitor_bypass` | 待测试 | |
| PROC GRAY + DUMP | Tag：`rtos_monitor_gray` | 待测试 | |
| PROC BINARY + THR 128 + DUMP | Tag：`rtos_monitor_binary` | 待测试 | |
| OV56RGB5 / Python 兼容性 | 检查 Python header、payload 和 CRC32 校验 | 待测试 | |

## 风险与约束

1. UART 仍采用 `HAL_UART_Receive()` 单字节轮询，连续突发命令依赖 CameraServiceTask 及时读取。
2. `PENDING` 状态绝对不能执行 `osDelay`，否则可能丢失连续字符。
3. STATUS 和 DUMP 共用 UART，必须由 CameraServiceTask 串行处理；DUMP 前和 DUMP 期间不能插入文本。
4. DUMP 使用阻塞式 UART 发送，CameraServiceTask 运行期间可能推迟低优先级 MonitorTask 的心跳，因此 `uptime_ms` 是心跳累计值，不是严格实时时钟。
5. 统计字段为 32 位无符号整数，长时间运行后允许自然回绕。
6. 当前不做 UART DMA + IDLE。
7. 当前不使用 StreamBuffer。
8. 当前不新增 Queue、Semaphore、Mutex、Event Flags 或 Software Timer。

## 阶段结论

本轮代码构建与静态检查已完成；仍需按上述步骤进行 COM4 板上验证，测试结果暂不填写。
