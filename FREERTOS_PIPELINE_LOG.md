# FreeRTOS 多任务化日志

## 阶段

Stage 7：FreeRTOS 最小多任务化

## 当前状态

1. STM32F429 + OV5640 图像采集链路已完成。
2. PC Dump 图像导出已完成。
3. AEC / AWB / 图像参数调试已完成。
4. 160x120 RGB565 双缓冲已完成。
5. 基础图像处理已完成。
6. UART CLI 在线调参已完成。
7. 本轮仅进行 Stage 7 最小任务化验证收尾，不新增其他功能。

## 已完成内容

1. STM32CubeMX 已启用 FreeRTOS。
2. 工程使用 CMSIS-RTOS v2。
3. CubeMX 已创建真实任务 `CameraServiceTa` 和 `MonitorTask`。
4. 已新增 `camera_rtos.h` / `camera_rtos.c` 模块。
5. `main.c` 已移除旧裸机 UART 命令循环。
6. CameraServiceTask 已接管 UART CLI / DUMP。
7. MonitorTask 只执行周期延时并维护心跳计数。

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

Image process:
Default mode = BYPASS
Default binary threshold = 128

CLI:
Default mode = BYPASS
Default binary threshold = 128
```

## 当前任务结构

### CameraServiceTask

- CubeMX 真实任务名称：`CameraServiceTa`。
- CubeMX 入口函数：`StartCameraServiceTask()`。
- 入口函数调用 `Camera_RTOS_CameraServiceTask(argument)`。
- 优先级：AboveNormal。
- 栈大小：8192 B。
- 独占 UART 命令接收、CLI 文本响应、DUMP 图像采集、图像处理和 OV56RGB5 发送。

### MonitorTask

- CubeMX 真实任务名称：`MonitorTask`。
- CubeMX 入口函数：`StartMonitorTask()`。
- 入口函数调用 `Camera_RTOS_MonitorTask(argument)`。
- 优先级：Low。
- 栈大小：2048 B。
- 每 1000 ms 执行一次 `monitor_tick_count++`。
- 不使用 UART，不访问 DCMI/DMA，不访问 frame buffer，不处理图像，不打印日志。

## UART 轮询接收迁移到 RTOS 后的 PENDING 延时问题

UART 是连续字节流。115200 波特率下，一个字符的传输时间约为 87 us，`HELP\r\n` 约 0.5 ms 即可发送完成。

当前工程没有使用 UART DMA，也没有使用 UART 中断环形缓冲。命令接收仍采用 `HAL_UART_Receive()` 单字节轮询，这种方式无法在任务未及时继续读取时保存完整的突发命令。

迁移到 RTOS 后，曾在已经收到部分命令的 `PENDING` 状态执行 `osDelay(1ms)`。延时期间 PC 已继续发送后续字符，USART 接收寄存器无法保存整条突发命令，可能产生 ORE 或字节丢失，最终导致 CLI 收到残缺字符串，DUMP 也无法完整识别。

当前修复规则如下：

1. `PENDING` 表示本轮已经收到字节但命令尚未完整，CameraServiceTask 立即继续轮询，不执行 `osDelay`。
2. `NONE` 表示本轮没有可处理的有效命令，CameraServiceTask 才执行 `osDelay(1U)` 让出 CPU。
3. `HAL_TIMEOUT` 只返回 `NONE`，不清 UART 错误，不清空已接收的行缓存。
4. 只有 `HAL_ERROR` 才清 UART 错误并重置行缓存。
5. 命令接收中间状态不主动延时。

## 数据流

```text
CameraServiceTask
    ->
HAL_UART_Receive 单字节轮询
    ->
PENDING：立即继续轮询
NONE：osDelay(1U)
CLI：Camera_CLI_HandleLine()
DUMP：进入图像导出流程
```

DUMP 数据流：

```text
DUMP
    ->
DCMI/DMA capture
    ->
back buffer
    ->
commit
    ->
front buffer 原图
    ->
BYPASS / GRAYSCALE / BINARY
    ->
back buffer 处理结果
    ->
commit
    ->
front buffer 处理结果
    ->
OV56RGB5 + header + payload + CRC
    ->
COM4
    ->
Python
```

## 关键约束

1. CameraServiceTask 独占 UART、CLI 和 DUMP。
2. `main.c` 不再运行旧命令循环。
3. MonitorTask 只维护心跳计数。
4. DUMP 前不输出文本。
5. OV56RGB5、header、payload、CRC 和发送顺序保持不变。
6. Python 工具保持不变。
7. 不新增 Queue、Semaphore、Mutex、StreamBuffer、Event Flags 或 Software Timer。
8. 不使用 malloc/free。
9. 不拆分摄像头采集、图像处理和 UART 发送任务。
10. 当前未实现 UART DMA + IDLE。
11. 新增或修改代码时，新增注释统一使用中文；已有英文注释暂不批量翻译，避免引入无关差异。

## 运行统计

```text
camera_service_loop_count
dump_success_count
dump_error_count
monitor_tick_count
last_error_code
```

## 测试方法

文本 CLI 使用 MobaXterm Serial：

```text
Serial port = COM4
Speed = 115200
Flow control = None
Data bits = 8
Stop bits = 1
Parity = None
Serial engine = PuTTY
```

图像 DUMP 使用：

```bash
python tools/pc_dump_rgb565.py --port COM4 --baud 115200 --tag <tag>
```

测试时不能由 MobaXterm 和 Python 同时占用 COM4。

## 测试结果

| 测试项 | 命令或方法 | Tag | 结果 | 说明 |
|---|---|---|---|---|
| HELP | `HELP` | - | 待填写 | |
| STATUS | `STATUS` | - | 待填写 | |
| DUMP BYPASS | `RESET` 后运行 Python DUMP | `rtos_bypass_default` | 待填写 | |
| PROC GRAY + DUMP | `PROC GRAY` 后运行 Python DUMP | `rtos_gray` | 待填写 | |
| PROC BINARY + THR 128 + DUMP | `PROC BINARY`、`THR 128` 后运行 Python DUMP | `rtos_binary_128` | 待填写 | |
| RESET + DUMP | `RESET` 后运行 Python DUMP | `rtos_reset_bypass` | 待填写 | |
| MonitorTask 心跳 | 观察 `monitor_tick_count` | - | 待填写 | |

## 后续计划

Stage 8 将在 Stage 7 基础上增加运行统计和 STATUS 输出增强，串口测试统一使用 COM4。

UART DMA + IDLE 可作为后续阶段计划，当前阶段未实现。

## 阶段结论

COM4 硬件验证结果待填写。
