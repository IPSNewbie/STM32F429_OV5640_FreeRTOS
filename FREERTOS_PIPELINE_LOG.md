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

---

## Stage 15A CommandQueue 引入与命令执行解耦

### 本阶段边界

Stage 15A 只引入统一命令值对象和 `CommandQueue`，没有增加业务功能，也没有拆分任务。
`CameraServiceTask` 仍是唯一业务执行者；`MonitorTask` 的优先级、栈大小、IWDG 策略和
职责均不改变。本阶段没有新增 Mutex、EventGroup、Task Notification 或其他 Task。

### 重构前

```text
UART RX DMA
    -> StreamBuffer
    -> CameraServiceTask
    -> UART dispatcher / 文本 parser
    -> 直接执行 CLI、DUMP、SD SNAPSHOT 或 binary image request
```

### 重构后

```text
UART RX DMA
    -> StreamBuffer
    -> CameraServiceTask 中的现有 parser
    -> CameraCommand_t
    -> Camera_CommandSubmit()
    -> CameraCommandQueue
    -> CameraServiceTask command dispatcher
    -> 执行原有 CLI、DUMP、SD SNAPSHOT 或 binary image request
```

parser 只负责识别命令、校验参数并生成值对象。合法命令中不保存原始行字符串、局部
parser buffer 指针或其他临时地址；FreeRTOS Queue 直接复制完整 `CameraCommand_t`。
语法错误仍由 parser 使用原有错误文本立即报告，不会形成业务命令。

### CommandQueue 设计

- 定义位置：`BSPDrivers/Inc/camera_command.h`。
- 实现位置：`BSPDrivers/Src/camera_command.c`。
- 队列深度：8。
- 元素类型：`CameraCommand_t`。
- 创建方式：`xQueueCreate()`；创建失败进入现有 `configASSERT` 故障机制。
- 提交方式：`Camera_CommandSubmit()` 内部使用 0 tick 的 `xQueueSend()`。
- 队列句柄：只在 `camera_command.c` 内部可见，parser 不直接访问句柄。
- 队列满：拒绝该命令、`command_drop_count++`、记录明确的最近提交错误，不阻塞、
  不复位，也不使用 malloc/free 保存命令。

内部最小统计为：

```text
command_submit_count
command_execute_count
command_drop_count
```

这些字段本阶段不加入 `STATUS`，现有 STATUS 文本保持不变。

### 命令执行与原业务复用

`CameraServiceTask` 在每个解析事件之后立即非阻塞 drain 队列，以保持原有 UART 回复
顺序。HELP、STATUS、PROC、THR、RESET、SD STATUS 和 SD SNAPSHOT 出队后仍调用
`camera_cli` 中的既有输出或业务 helper；DUMP 仍调用公共 RGB565 准备与
`Camera_PC_Dump_SendFrame()` 路径；SD SNAPSHOT 仍调用
`Camera_SDStorage_SaveSnapshotFrame()`。没有复制 DCMI、图像处理、FatFs 或发送实现。

binary image request 本阶段也进入 CommandQueue。命令值对象只保存执行所需的 `seq`；
请求 parser、14 字节 wire protocol、OV56RGB5 响应、payload CRC32、`frame_id` 递增规则
和 binary 请求无 ASCII 错误插入的行为均不改变。

### 保持不变的系统行为

1. 最终 HELP 仍固定为原有 8 项，顺序和文本不变。
2. STATUS 字段和输出文本不变。
3. DUMP、OV56RGB5、CRC32 和 Python 工具不变。
4. SD SNAPSHOT、BMP24、IMGxxxx.BMP 命名和 SDIO/DVP takeover/restore 不变。
5. DCMI/DMA ISR、SDIO DMA/IRQ、OV5640 初始化表和 DVP mask 不变。
6. 所有业务仍由单个 `CameraServiceTask` 串行执行，因此本阶段不需要新增 Mutex。

### 静态验证

- `git diff --check`：PASS。
- `cmake --build build/Debug`：PASS，无新增编译 warning。
- `CameraCommand_t`：4 B；8 个队列槽位共 32 B。
- Queue 控制块：80 B；heap_4 分配头：8 B；`xQueueCreate()` 运行时共占既有 heap 120 B。
- `arm-none-eabi-size`：text=93360 B，data=104 B，bss=156944 B。
- FLASH（text+data）：93464 B；相对重构前 92252 B 增加 1212 B。
- 链接 RAM（data+bss）：157048 B；相对重构前 157024 B 增加 24 B。
- 硬件测试：未执行；Stage 15A 按要求只做代码修改、构建和静态检查。

---

## Stage 15B CommTask / ControlTask 拆分

### 本阶段边界

Stage 15B 只把 Stage 15A 的 `CameraServiceTask` 拆为通信入口 `CommTask` 和串行业务
执行者 `ControlTask`，保留原 `MonitorTask`。本阶段没有增加业务命令，没有创建
`CaptureTask`、`ProcessTask` 或 `StorageTask`，也没有引入 Mutex、EventGroup、Task
Notification。DCMI、图像处理、UART 发送和 SD 保存仍由 `ControlTask` 串行调用，真正的
采集任务拆分留到 Stage 15C。

### 拆分前后

拆分前：

```text
UART RX DMA -> StreamBuffer -> CameraServiceTask
    -> parser -> CommandQueue -> CameraServiceTask dispatcher -> 原有业务
```

拆分后：

```text
UART RX DMA -> StreamBuffer -> CommTask
    -> UART dispatcher / parser -> CameraCommand_t -> CommandQueue
    -> ControlTask -> 原有 CLI / DUMP / SD SNAPSHOT / binary response
```

`CameraServiceTask` 不再创建。`CommTask` 是 StreamBuffer、UART dispatcher、文本行 parser
和 binary request parser 的唯一任务上下文，只生成并提交命令，不执行 DCMI、图像处理、
SD 或 UART 回复。`ControlTask` 使用 `portMAX_DELAY` 阻塞等待 `CommandQueue`，出队后复用
既有业务 helper；没有命令时不轮询。`CommTask` 继续使用 UART StreamBuffer 的 100 ms
有界阻塞，以便处理 binary inter-byte timeout 和 UART DMA 恢复请求。

### 优先级与栈

| Task | CMSIS-RTOS2 优先级 | 栈大小 | 说明 |
|---|---:|---:|---|
| CommTask | `osPriorityAboveNormal` | 2048 B | 高于业务执行者，及时搬运和解析 UART 输入 |
| ControlTask | `osPriorityNormal` | 8192 B | 继承已验证的旧 CameraServiceTask 业务栈，不在本阶段冒险缩栈 |
| MonitorTask | `osPriorityLow` | 2048 B | 保持原配置与监控职责 |

Stage 15A 的 `CommandQueue` 保持深度 8，`CameraCommand_t` 保持 4 B。相对 Stage 15A，
`ControlTask` 替换旧任务且栈大小相同；新增 `CommTask` 的动态 FreeRTOS heap 预算约为
2160 B（2048 B 栈加 TCB 和 heap_4 元数据/对齐）。

### 串口输出与命令顺序

为避免两个任务并发阻塞发送 USART1，`CommTask` 不直接输出文本。合法命令和已有 CLI
语法错误都转换为值对象进入同一 `CommandQueue`；语法错误使用
`CAMERA_CMD_CLI_ERROR` 保存错误类别，由 `ControlTask` 输出原有的 `ERR bad PROC arg`、
`ERR bad THR arg` 或 `ERR unknown command`。因此无需新增 UART Mutex，且 CLI 回复与业务
回复继续按队列顺序串行输出。队列满仍是 0 tick 拒绝并更新既有 drop/last-error 统计。

binary 链路保持为：

```text
UART DMA -> StreamBuffer -> CommTask dispatcher
    -> CAMERA_CMD_IMAGE_REQUEST(seq) -> CommandQueue -> ControlTask
    -> 既有公共帧准备路径 -> OV56RGB5 response
```

14 字节请求格式、`seq`、OV56RGB5 header/payload/CRC32、`frame_id` 规则以及 binary 流中
不插入 ASCII 错误文本的行为均未改变。HELP、DUMP、SD SNAPSHOT、BMP24、IMGxxxx.BMP
命名、SDIO/DVP takeover/restore 和 Python 工具也未改变。

### STATUS 与 IWDG

原 `stack_camera_min` 拆为 `stack_comm_min` 和 `stack_control_min`，并保留
`stack_monitor_min`；工程工具没有依赖旧字段。统计结构同时分别记录 Comm/Control 的
heartbeat、heartbeat age 和栈低水位。

IWDG 继续由 MonitorTask 统一喂狗。CommTask 和 MonitorTask 沿用心跳年龄检查；
ControlTask 阻塞等待空队列时设置明确的 waiting 状态，此时不因没有命令而误判超时，
一旦出队进入业务执行则恢复 Control 心跳年龄检查。没有增加 EventGroup 或 Task
Notification；更细的任务健康监督留待后续阶段。

### 静态验证

- `git diff --check`：PASS。
- `cmake --build build/Debug`：PASS；Stage 15B 修改无新增 warning。因头文件注释触发
  `OV5640.c` 重编译时，仍可见 `ov5640cfg.h` 原有的 `-Wmissing-braces` warning，未在
  本阶段改动初始化表。
- `CameraCommand_t`：4 B；Queue 深度仍为 8，队列内存不变。
- `arm-none-eabi-size`：text=93892 B，data=104 B，bss=156976 B。
- FLASH（text+data）：93996 B；链接器报告 94000 B，相对 Stage 15A 增加 532 B。
- 链接 RAM（data+bss）：157080 B，相对 Stage 15A 增加 32 B。
- 硬件测试：未执行；本阶段按要求只做代码修改、构建和静态检查。

---

## Stage 15C CaptureTask 与 DCMI Task Notification

### 本阶段边界

Stage 15C 只在 Stage 15B 的 CommTask、CommandQueue、ControlTask、MonitorTask 架构上增加
CaptureTask、深度为 1 的 CaptureRequestQueue，以及 DCMI ISR 到 CaptureTask 的 Task
Notification。没有增加 ProcessTask、StorageTask、EventGroup 或 Mutex。

### 采集链路

```text
ControlTask -> Camera_RTOS_PrepareRgb565Frame()
    -> Camera_CaptureRequestFrame() -> CaptureRequestQueue
    -> CaptureTask -> Camera_DCMI_StartSnapshotToBuffer()
    -> HAL_DCMI_Start_DMA() -> DCMI frame ISR
    -> vTaskNotifyGiveFromISR() -> CaptureTask
    -> task notification completion -> ControlTask
    -> commit -> image process -> commit
```

CaptureTask 使用 `ulTaskNotifyTake()` 有界等待原始帧完成，并区分 OK、启动失败、超时和 HAL
错误。CaptureTask 与 ControlTask 在每次新请求前清理各自可能残留的 notification；完成结果通过
一对一 Task Notification 返回请求者。

### Ownership

DCMI DMA 活动期间，当前 back buffer 只由 CaptureTask/硬件拥有。CaptureTask 在完整帧、超时或
错误后停止 DCMI/DMA 并释放 ownership；随后 ControlTask 才按 Stage 15B 原顺序执行第一次
commit、BYPASS/GRAY/BINARY 图像处理和第二次 commit。DUMP 与 SD SNAPSHOT 仍只读取最终
front frame，因此不需要 Mutex。

### 任务与健康状态

| Task | CMSIS-RTOS2 优先级 | 栈大小 |
|---|---:|---:|
| CommTask | `osPriorityAboveNormal` | 2048 B |
| ControlTask | `osPriorityNormal` | 8192 B |
| CaptureTask | `osPriorityAboveNormal` | 1024 B |
| MonitorTask | `osPriorityLow` | 2048 B |

MonitorTask 读取 CaptureTask 的 stack high-water mark；`STATUS` 只新增
`stack_capture_min`。CaptureTask 保留内部 heartbeat，但本阶段没有改变 Stage 15B 的 IWDG gate。

### 保持不变

SDIO/FatFs、SDIO takeover、OV5640 DVP mask、BMP/IMGxxxx.BMP、OV56RGB5、UART wire
protocol、Python 工具、图像算法和 HELP 文本均未修改。没有新增动态帧内存或大帧队列。

### 静态验证

- `git diff --check`：PASS。
- `cmake --build build/Debug --parallel`：PASS。
- 链接 RAM：157112 B（79.91%）。
- 链接 FLASH：95360 B（9.09%）。
- 硬件测试：未执行。
