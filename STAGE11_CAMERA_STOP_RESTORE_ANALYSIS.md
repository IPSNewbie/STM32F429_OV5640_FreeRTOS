# Stage 11B-4 相机停止/恢复最小改动点梳理

## 1. 本轮目标

本轮只阅读和梳理现有源码，不修改任何固件代码。目标是确认后续真正实现 `SNAPSHOT PREPARE` / `SNAPSHOT RESTORE` 时，DCMI、DMA、双缓冲、DUMP、二进制请求、RTOS 和 GPIO 之间的实际调用关系，并找出最小、可回滚的改动点。

当前分支实际启用 `CAMERA_MODE_PC_DUMP_RGB565`。该模式不是 DCMI 连续采集，而是收到每个图像请求后启动一次 160×120 RGB565 快照，完成后停止 DCMI/DMA。因此本文首先针对当前 PC DUMP 模式分析；LCD 连续显示模式的恢复路径不同，不能直接套用本文的最小流程。

## 2. 当前摄像头采集链路

### 2.1 初始化和启动位置

| 环节 | 文件和函数 | 源码实际行为 |
| --- | --- | --- |
| 应用初始化入口 | `Core/Src/main.c`：`Camera_Application_Init()` | 初始化 OV5640 后调用 `Camera_FrameBuffer_Init()`、`Camera_CLI_Init()` 和 `Camera_DCMI_Init()`。当前 `CAMERA_MODE` 为 `CAMERA_MODE_PC_DUMP_RGB565`，启动阶段不调用 LCD 连续采集入口。 |
| DCMI GPIO 初始化 | `BSPDrivers/Src/camera_dcmi_dma.c`：`Camera_DCMI_GPIO_Init()` | 将 PA6、PB7/8/9、PC6/7/8/9/11、PD3、PH8 配置为 `GPIO_AF13_DCMI`。当前搜索范围内未找到用户实现的 `HAL_DCMI_MspInit()`。 |
| DCMI 外设初始化 | `BSPDrivers/Src/camera_dcmi_dma.c`：`Camera_DCMI_Init()` | 使用全局句柄 `g_camera_dcmi`，配置硬件同步、PCLK 上升沿、低有效 VSYNC/HREF、8 位数据和非 JPEG 模式，然后调用 `HAL_DCMI_Init()`。仅使能帧中断，明确关闭 LINE、VSYNC、ERR 和 OVR 中断。 |
| DCMI DMA 句柄 | `BSPDrivers/Src/camera_dcmi_dma.c` | 使用全局 `g_camera_dma`；项目没有名为 `hdma_dcmi` 的句柄。DCMI DMA 固定为 `DMA2_Stream1`、`DMA_CHANNEL_1`。 |
| 单帧 DMA 初始化 | `BSPDrivers/Src/camera_dcmi_dma.c`：`Camera_DCMI_StartSnapshotToBuffer()` | 每次快照都重新配置并 `HAL_DMA_DeInit()` / `HAL_DMA_Init()`，使用外设到内存、地址递增、32 位对齐、`DMA_NORMAL`，随后通过 `__HAL_LINKDMA()` 绑定到 `g_camera_dcmi`。 |
| 启动单帧采集 | `BSPDrivers/Src/camera_rtos.c`：`Camera_RTOS_CaptureProcessAndSend()` | 收到图像请求后调用 `Camera_DCMI_StartSnapshotToBuffer()`；底层最终调用 `HAL_DCMI_Start_DMA(..., DCMI_MODE_SNAPSHOT, ...)`。 |
| DMA 目标缓冲区 | `BSPDrivers/Src/camera_pc_dump.c`：`Camera_PC_Dump_GetBufferAddress()` | 返回 `Camera_FrameBuffer_GetBackBuffer()`，所以 DCMI DMA 只写 back buffer。传输量由 `Camera_PC_Dump_GetWordCount()` 返回，为 9600 个 32 位 word。 |
| 帧规格 | `BSPDrivers/Inc/camera_frame_buffer.h`、`BSPDrivers/Inc/camera_pc_dump.h` | 160×120、RGB565、每像素 2 字节，共 38400 字节；双缓冲数量为 2。 |
| 帧完成通知 | `BSPDrivers/Src/camera_dcmi_dma.c`：`HAL_DCMI_FrameEventCallback()` | 当 `s_camera_snapshot_active` 为 1 时，清除 active 并置位 `s_camera_snapshot_done`。当前搜索范围内未找到 `HAL_DCMI_ErrorCallback()` 或独立 DMA 完成回调。 |
| 等待和停止 | `BSPDrivers/Src/camera_rtos.c`：`Camera_RTOS_CaptureProcessAndSend()` | 轮询 `Camera_DCMI_IsSnapshotDone()`，最长等待 3000 ms；启动失败、超时和正常完成都会调用 `Camera_DCMI_Stop()`。 |
| 当前停止实现 | `BSPDrivers/Src/camera_dcmi_dma.c`：`Camera_DCMI_Stop()` | 直接清除 `DCMI_CR_CAPTURE` 并禁用 `g_camera_dma`，无返回值、无 DMA 停止确认，也不调用 `HAL_DCMI_Stop()` / `HAL_DMA_Abort()`。 |

### 2.2 帧缓存 commit/swap 路径

双缓冲实现在 `BSPDrivers/Src/camera_frame_buffer.c`：

1. `Camera_FrameBuffer_Init()` 将 front index 置 0、back index 置 1。
2. `Camera_FrameBuffer_GetBackBuffer()` 返回 DMA 或图像处理的写入目标。
3. 单帧 DCMI 完成并停止后，`Camera_RTOS_CaptureProcessAndSend()` 调用 `Camera_FrameBuffer_CommitBackBuffer()`。
4. `Camera_FrameBuffer_CommitBackBuffer()` 交换 front/back 索引，使刚采集的完整 back buffer 成为 front buffer。
5. `Camera_ImageProcess_ApplyToFrameBuffer()` 读取当前 front，处理或旁路复制到新的 back，再次调用 `Camera_FrameBuffer_CommitBackBuffer()`。
6. 因此最终发送的 front buffer 是处理后或旁路复制后的完整帧。

当前 `Camera_FrameBuffer_GetFrontFrame()` 只填充地址、宽、高和大小，并始终返回 `CAMERA_FB_OK`；模块内没有“front 是否已提交过有效帧”的标志。因此上电后尚未完成首次捕获时，接口也会返回一个非空 front 地址，不能仅凭返回值认定其中已有有效图像。

### 2.3 DUMP 和二进制请求路径

文本 DUMP 路径：

```text
USART1 RX DMA / StreamBuffer
  -> Camera_RTOS_CameraServiceTask()
  -> CameraUartDispatcher_FeedByte()
  -> Camera_RTOS_ProcessTextByte()
  -> Camera_PC_Dump_FeedCommandByte()
  -> CAMERA_PC_DUMP_CMD_DUMP
  -> Camera_RTOS_ProcessDumpRequest()
  -> Camera_RTOS_CaptureProcessAndSend()
```

二进制图像请求路径：

```text
USART1 RX DMA / StreamBuffer
  -> Camera_RTOS_CameraServiceTask()
  -> CameraUartDispatcher_FeedByte()
  -> ImageRequestProtocol_FeedByte()
  -> CAMERA_UART_DISPATCH_IMAGE_REQUEST
  -> Camera_RTOS_ProcessDispatchEvent()
  -> Camera_RTOS_ProcessDumpRequest()
  -> Camera_RTOS_CaptureProcessAndSend()
```

文本 DUMP 和二进制图像请求最终共用同一个静态函数 `Camera_RTOS_ProcessDumpRequest()`，不存在另一条直接读取图像的二进制发送路径。

`Camera_RTOS_CaptureProcessAndSend()` 在 DCMI 停止、两次 commit/swap 和图像处理完成后调用 `Camera_PC_Dump_SendFrame()`。`Camera_PC_Dump_SendFrame()` 通过 `Camera_FrameBuffer_GetFrontFrame()` 获取 front buffer，计算 CRC 并发送 38400 字节 RGB565 payload。

### 2.4 PC8、PC9、PC11 与 DCMI 的关系

`BSPDrivers/Src/camera_dcmi_dma.c` 的引脚注释和 `Camera_DCMI_GPIO_Init()` 明确给出以下映射：

| MCU 引脚 | 当前 DCMI 信号 | SDIO 接管时的信号 |
| --- | --- | --- |
| PC8 | OV_D2 / DCMI_D2 | SDIO_D0 |
| PC9 | OV_D3 / DCMI_D3 | SDIO_D1 |
| PC11 | OV_D4 / DCMI_D4 | SDIO_D3 |

`Camera_DCMI_GPIO_Init()` 将包含这三个引脚在内的 PC6、PC7、PC8、PC9、PC11 一次性配置为 `GPIO_AF13_DCMI`。`Core/Src/main.c` 先调用 CubeMX 的 `MX_GPIO_Init()`，随后在 `Camera_Application_Init()` 中调用自定义 `Camera_DCMI_Init()`，真正的 DCMI AF13 配置发生在后者内部。

当前搜索范围内未找到释放 PC8、PC9、PC11 的函数，也未找到从 SDIO 恢复这三个引脚的专用函数。后续恢复阶段可以复用或拆分 `Camera_DCMI_GPIO_Init()`，但必须避免在 SDIO 尚未退出时整组重配 DCMI 引脚。

## 3. 当前 DUMP 与采集并发关系

### 3.1 源码可确认的并发边界

1. 文本命令和二进制请求都由同一个 `CameraServiceTask` 串行处理。`Camera_RTOS_ProcessDumpRequest()` 执行期间，该任务不会开始另一个 DUMP；后续 UART 字节只能先由 USART1 RX DMA 收入 StreamBuffer，等待当前流程结束后再处理。
2. DCMI DMA 在采集阶段写 back buffer，不写 front buffer。
3. 当前流程在 DMA 快照完成后先调用 `Camera_DCMI_Stop()`，再执行第一次 commit/swap。
4. 图像处理读取 front、写 back，并在处理完成后再次 commit/swap。
5. UART DUMP 最后只读取 front buffer。发送开始时 DCMI/DMA 已停止，所以当前实现中不存在“UART 正在读 front、DCMI 同时写同一 front”的路径。

### 3.2 停止后 front buffer 能否继续保存

只要满足以下条件，停止 DCMI 后 front buffer 仍可用于 SD 保存：

- 已完成一次成功采集、commit 和图像处理 commit；
- 暂停期间禁止新的 DUMP 或二进制请求启动下一次采集；
- SD 写入完成前不调用会再次修改或交换双缓冲的流程；
- 保存操作读取的是暂停时确认的 front 地址和 38400 字节长度。

当前源码没有 front 有效标志，也没有 buffer 锁或引用计数。因此不能只调用 `Camera_FrameBuffer_GetFrontFrame()` 就断言帧有效；真正的 `SNAPSHOT PREPARE` 应负责采集并固定一帧，或者新增明确的有效帧状态。

### 3.3 是否需要额外 snapshot buffer

基于当前源码，如果 SD 写入与相机暂停处于同一串行状态机中，并且写卡结束前不恢复采集，则 front buffer 不会被 DMA 覆盖，首版 raw 保存不必额外复制 38400 字节 snapshot buffer。

如果未来采用异步 SD 写任务、允许写卡期间恢复采集、允许图像处理再次 commit，或存在其他任务同时访问双缓冲，则仅持有 front 指针不再安全。届时需要二选一：

1. 增加 front buffer 所有权/锁定机制，写卡完成后释放；或
2. 在恢复采集前复制到独立 snapshot buffer。

## 4. 停止相机需要的最小动作

针对当前 PC DUMP 单帧模式，后续真正实现 `SNAPSHOT PREPARE` 的最小动作建议如下：

1. **进入软件互斥状态**：只允许 `IDLE` 进入 PREPARE，设置 busy/prepare 状态，拒绝新的 DUMP、二进制图像请求和重复 SD 保存请求。
2. **确认当前请求上下文**：当前 CLI、文本 DUMP 和二进制请求均在 `CameraServiceTask` 中串行执行，所以正常情况下 PREPARE 开始时不会有另一个 DUMP 正在函数栈中运行；仍应保留 busy 状态，为以后异步任务扩展提供保护。
3. **采集并固定一帧**：将现有 `Camera_RTOS_CaptureProcessAndSend()` 拆分为“捕获并处理到 front”和“发送 front”两段。PREPARE 复用前一段，但不发送 UART 图像。
4. **等待帧完成**：继续使用现有 3000 ms 超时，同时不能只看 `s_camera_snapshot_done`；应确认 DCMI capture 已停止、DMA Stream 已禁用或传输计数满足完整帧条件。
5. **停止 DCMI/DMA**：调用增强后的低层停止接口，清除 capture、停止 DMA，并返回可检查的结果。当前 `Camera_DCMI_Stop()` 没有返回值和停止确认，不足以直接作为最终接管条件。
6. **完成 commit/swap**：只有确认完整帧后才能 commit；图像处理成功后再次 commit，使最终 front 对应准备保存的完整图像。
7. **确认有效 front**：记录有效帧标志、宽 160、高 120、长度 38400 和固定 front 地址。
8. **进入暂停状态**：全部成功后才设置 `camera_control_state = CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED`，并设置 `frame_buffer_ready = 1`。
9. **开放 SD 接管**：只有 CAMERA_PAUSED、DMA 已停止且 front 有效时，才允许 `SD TAKEOVER ENTER` 进入真实接管流程。
10. **失败回滚**：任何步骤失败都应清除 busy 状态，保持或恢复 DCMI 引脚，并确保后续 DUMP 仍可重新启动单帧采集。

当前 active 模式本来就在每次快照后停止 DCMI/DMA，所以 PREPARE 的重点不是暂停持续流，而是“捕获一帧但不发送、确认停止、冻结 front、禁止下一次采集”。

## 5. 恢复相机需要的最小动作

针对当前 PC DUMP 单帧模式，后续真正实现 `SNAPSHOT RESTORE` 的最小动作建议如下：

1. 确认文件已经同步并关闭，FATFS 不再访问 SD 卡。
2. 完成 `SD TAKEOVER EXIT`，停止 SDIO 对冲突引脚的使用。
3. 将 PC8、PC9、PC11 恢复为 `GPIO_AF13_DCMI`。现有 `Camera_DCMI_GPIO_Init()` 可重配全部 DCMI 引脚，但当前没有只恢复冲突引脚的专用接口。
4. 检查 `g_camera_dcmi`、`g_camera_dma`、`s_camera_snapshot_active` 和完成标志是否处于下一次快照可用状态；必要时清理残留标志。
5. 当前 PC DUMP 模式无需立即重启连续 DCMI DMA；下一次 DUMP 会在 `Camera_DCMI_StartSnapshotToBuffer()` 中重新配置 DMA 并启动一帧采集。
6. 清除 CAMERA_PAUSED/busy 状态，恢复 `CameraServiceTask` 对文本 DUMP 和二进制图像请求的处理能力。
7. 保留或清除 `frame_buffer_ready` 必须由状态机明确：若允许继续查询最近一次保存帧可保留有效标志；若其只表示“当前冻结帧”则在恢复时清零。
8. 回归 `basic`、`pc_dump`、`repeat 20/20`、IWDG、Hook 和 UART DMA 状态。

LCD 连续显示模式使用 `Camera_DCMI_DMA_ConfigToLCD()`、`Camera_DCMI_StartToLCD()` 和循环 DMA，恢复动作明显不同。后续若要支持 LCD 模式，必须单独保存原模式和显示窗口参数，不能用 PC DUMP 模式的“等待下一次请求再启动”策略。

## 6. 后续建议新增的内部接口

本节只提出接口建议，不在 Stage 11B-4 新增声明或实现。

### 6.1 RTOS 编排接口

建议放在 `BSPDrivers/Inc/camera_rtos.h` 和 `BSPDrivers/Src/camera_rtos.c`：

```c
uint32_t Camera_RTOS_CapturePauseForSnapshot(void);
uint32_t Camera_RTOS_CaptureResumeAfterSnapshot(void);
uint32_t Camera_RTOS_CaptureIsPaused(void);
uint32_t Camera_RTOS_CaptureHasValidFrame(void);
```

理由：

- `CameraServiceTask` 是文本 DUMP、二进制请求和 CLI 的单一业务执行上下文；
- `Camera_RTOS_ProcessDumpRequest()`、捕获超时、commit、图像处理和发送流程均在该文件中；
- 暂停标志必须在 `Camera_RTOS_ProcessDumpRequest()` 入口处同时拦截文本和二进制请求；
- 可以在不修改 UART 协议的前提下复用现有统计和错误返回路径。

建议将当前静态 `Camera_RTOS_CaptureProcessAndSend()` 拆为两个仍保持文件内私有的步骤：

```c
static uint32_t Camera_RTOS_CaptureAndProcessToFront(void);
static uint32_t Camera_RTOS_SendFrontFrame(void);
```

现有 DUMP 依次调用两步，PREPARE 只调用第一步并进入暂停状态，可避免复制现有采集代码。

### 6.2 低层 DCMI/DMA 状态接口

建议放在 `BSPDrivers/Inc/camera_dcmi_dma.h` 和 `BSPDrivers/Src/camera_dcmi_dma.c`：

```c
uint32_t Camera_DCMI_StopSnapshotAndConfirm(void);
uint32_t Camera_DCMI_IsSnapshotIdle(void);
uint32_t Camera_DCMI_ReleaseConflictPins(void);
uint32_t Camera_DCMI_RestoreConflictPins(void);
```

理由：该模块拥有 `g_camera_dcmi`、`g_camera_dma`、快照 active/done 标志和 `GPIO_AF13_DCMI` 配置，适合集中完成低层停止确认与引脚恢复，避免 `camera_snapshot_control`、SD 模块和 RTOS 模块分别直接操作寄存器。

`ReleaseConflictPins()` 只负责让 DCMI 放弃 PC8、PC9、PC11；SDIO AF12 配置仍应由后续 SDIO 接管实现负责。这样可以保持 DCMI 与 SDIO 的引脚所有权边界清晰。

### 6.3 帧有效性接口

建议放在 `BSPDrivers/Inc/camera_frame_buffer.h` 和 `BSPDrivers/Src/camera_frame_buffer.c`：

```c
uint32_t Camera_FrameBuffer_HasValidFrontFrame(void);
void Camera_FrameBuffer_InvalidateFrontFrame(void);
```

理由：当前 `Camera_FrameBuffer_GetFrontFrame()` 无法区分“地址存在”和“已提交完整帧”。有效性属于帧缓冲自身状态，应在初始化时清零、成功 commit 后置位，而不是由 CLI 根据指针非空推断。

### 6.4 与 camera_snapshot_control 的连接

现有 `Camera_SnapshotControl_RequestPrepare()` / `RequestRestore()` 建议继续作为 CLI 可见状态机入口，但实际硬件编排应委托给上述 RTOS 接口。只有 RTOS 捕获、低层停止确认和有效帧检查全部成功后，才更新成功计数和 CAMERA_PAUSED；恢复失败时必须保留 ERROR 状态和可诊断错误码。

## 7. 风险点

1. DCMI 帧事件与 DMA 真正完成的时序不一致，过早停止或 commit 可能得到半帧。
2. 当前完成标志由 `HAL_DCMI_FrameEventCallback()` 设置，未找到独立 DMA 完成回调；仅看该标志可能不足以证明 DMA 已完全静止。
3. DCMI ERR/OVR 中断当前被关闭，且未找到 `HAL_DCMI_ErrorCallback()`；异常可能只能依赖 3000 ms 超时暴露。
4. 当前 `Camera_DCMI_Stop()` 无返回值、不确认 DMA EN/NDTR，也不清理所有快照状态，失败路径可能留下残余状态。
5. frame buffer 没有有效帧标志，上电后 front 地址非空不等于已有完整图像。
6. front/back 索引交换没有锁；当前单任务串行成立，但未来异步 SD 写任务会引入并发风险。
7. DUMP、二进制请求、SNAPSHOT 和 SD 保存若缺少统一 busy/paused 门控，可能产生状态冲突或重复采集。
8. PC8、PC9、PC11 从 DCMI 切换到 SDIO 后若恢复失败，摄像头数据位 D2、D3、D4 将丢失，图像会严重损坏。
9. SDIO 初始化或写卡失败后若只返回错误而不优先恢复相机，系统可能长期停留在不可采集状态。
10. 反复切换 DCMI/SDIO 可能残留 GPIO AF、DMA 标志、中断使能或 HAL 句柄状态。
11. SD/FATFS 写入时间不可控；若在 `CameraServiceTask` 中长时间阻塞，6 秒相机心跳限制可能导致 MonitorTask 跳过 IWDG 刷新。
12. UART RX DMA 会在相机或 SD 操作期间继续接收，StreamBuffer 可能积压；恢复后必须按顺序处理或明确拒绝旧请求。
13. LCD 连续显示和 PC DUMP 单帧模式的停止/恢复路径不同，未区分模式会造成错误重启方式。
14. 如果未来保存 BMP，需要正确处理 RGB565 位掩码、字节序、BMP 文件头和行对齐。
15. 如果未来保存 RAW，需要在文件名或元数据中固定宽、高、RGB565 格式和字节序，PC 端必须按 38400 字节解析。

## 8. Stage 11B-5 建议计划

建议下一步执行：

```text
Stage 11B-5：实现相机停止/恢复软件状态保护，不切换 SDIO
```

建议目标：

1. 在 `camera_rtos` 中增加 paused/busy/valid-frame 软件状态和只读查询接口。
2. 在文本 DUMP 与二进制请求共用的 `Camera_RTOS_ProcessDumpRequest()` 入口增加暂停门控，先验证禁止新采集请求。
3. 将捕获/处理与 UART 发送拆分成两个内部步骤，使 PREPARE 可以固定一帧而不发送。
4. 让 `SNAPSHOT PREPARE` / `RESTORE` 接入软件状态机，但暂不释放 PC8、PC9、PC11，也不初始化 SDIO。
5. 为非法状态转换、重复 PREPARE、未暂停 RESTORE 和恢复失败定义错误码。
6. 继续执行 `basic`、`pc_dump`、`repeat 20/20`、IWDG、Hook 和 UART DMA 回归。

不建议 Stage 11B-5 直接做真实 GPIO/SDIO 切换。依据是当前停止接口缺少确认、frame buffer 缺少有效标志、捕获与发送仍耦合；先验证软件门控和状态转换，可以将后续硬件风险限制在更小范围。

## 9. 本轮结论

Stage 11B-4 只完成相机停止/恢复最小改动点梳理，没有修改固件代码。

源码确认当前 PC DUMP 模式按请求执行单帧采集：`Camera_DCMI_StartSnapshotToBuffer()` 将 DMA 指向 back buffer，`HAL_DCMI_FrameEventCallback()` 通知完成，`Camera_DCMI_Stop()` 停止采集，随后通过两次 commit/swap 得到最终 front buffer，文本 DUMP 和二进制请求再共用 `Camera_PC_Dump_SendFrame()` 发送该 front buffer。

后续真正实现前，需要先确认并强化 DCMI/DMA 停止完成条件、front 有效帧状态、捕获与发送拆分、DUMP 暂停门控，以及 PC8、PC9、PC11 的 DCMI 复用恢复路径。建议先进入 Stage 11B-5 完成软件状态保护，再实施真实引脚切换和 SDIO 初始化。
