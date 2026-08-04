# FreeRTOS运行健康监控日志

## 阶段

Stage 10 Round 1修正：统一栈余量字段语义

## 本阶段目的

- 通信成功不代表系统长时间运行一定安全。
- 任务栈不足可能造成随机崩溃。
- Heap不足可能导致后续内核对象创建失败。
- 本轮统一栈watermark统计语义，不修改采样位置、任务、Heap或保护配置。

## 当前任务

| 任务 | 优先级 | 配置栈大小 |
| --- | --- | ---: |
| CameraServiceTask（线程名CameraServiceTa） | osPriorityAboveNormal | 8192 B（2048 * 4） |
| MonitorTask | osPriorityLow | 2048 B（512 * 4） |

以上配置来自`Core/Src/freertos.c`中的当前任务属性，未在本轮修改。

## 栈余量是什么

- 栈总大小是创建任务时为该任务配置的栈容量。
- 当前CMSIS-FreeRTOS实现的`osThreadGetStackSpace()`基于FreeRTOS stack high-water mark。
- 该watermark表示任务启动以来，栈最深使用时仍未被触及的最小剩余空间，不是调用时刻的瞬时剩余栈。
- 返回的最小剩余栈数值只能保持不变或下降；数值越小，说明任务曾经越接近栈溢出。
- 当前工程没有使用可移植接口获得真正的瞬时栈余量，因此不提供“当前剩余栈”字段。

## Heap余量是什么

- `free_heap_bytes`是采样时FreeRTOS Heap的当前剩余字节数。
- `min_ever_free_heap_bytes`是FreeRTOS启动以来曾出现过的最小Heap剩余字节数。
- 当前free heap可以随内核对象申请和释放而变化；min ever free heap只会保持或下降。
- 当前工程`configTOTAL_HEAP_SIZE`为32768 B，本轮未修改。

## 复位原因诊断

- Stage 10 Round 1.1B在`main()`进入后、`HAL_Init()`之前保存`RCC->CSR`，随后立即清除硬件复位标志；后续两次日志都只解析这份启动快照，不重新读取已清除的寄存器。
- UART1和现有`bsp_log`初始化完成后输出`RESET_CAUSE`；摄像头、DCMI和Camera RTOS应用初始化完成、FreeRTOS内核初始化前输出`RESET_CAUSE_READY`。第二条日志用于提高PC端捕获启动诊断的机会。
- 输出保留原始CSR十六进制值，并分别给出`PIN`、`POR`、`BOR`、`SOFTWARE`、`IWDG`、`WWDG`和`LOW_POWER`的0或1状态；多个标志可能同时存在，不能假设只有一个复位原因。
- `PIN=1`通常表示NRST引脚或外部复位路径，`SOFTWARE=1`表示软件系统复位，`IWDG=1`或`WWDG=1`表示对应看门狗复位，`POR=1`或`BOR=1`表示上电或电压相关复位。
- 标志只能证明MCU记录到的复位来源，最终结论必须依据真实板测值，并结合两条日志是否出现判断是否发生完整芯片复位。
- 本轮尚未启用IWDG，也未主动调用系统复位，因此不能预设实际标志结果；板上结果待测试。

## 实现方式

- 栈余量使用当前CMSIS-RTOS2实现的`osThreadGetStackSpace(osThreadGetId())`读取。
- 当前工程的`osThreadGetStackSpace()`内部调用`uxTaskGetStackHighWaterMark()`，再乘以`sizeof(StackType_t)`；本端口`StackType_t`为`uint32_t`。
- CMSIS-RTOS2接口已经完成字节转换，本模块直接将返回值写入对应的`*_stack_min_free_bytes`字段，不再进行第二层min比较。
- CameraServiceTask在自身上下文中，于UART DMA初始化完成后立即读取watermark；随后在正常读取/分发循环处理结束时采用简单时间差判断，约每1000 ms读取一次；DUMP完成后立即读取，以取得DUMP过程中可能产生的新历史最低值。该限频未新增软件定时器，也不会按每个32 B StreamBuffer块采样。
- MonitorTask在自身上下文中，每次1000 ms循环读取自身stack watermark、当前Heap和历史最小Heap。
- Heap使用`xPortGetFreeHeapSize()`和`xPortGetMinimumEverFreeHeapSize()`，返回值保存为`uint32_t`字节数。

## 健康统计字段

| 字段 | 含义 |
| --- | --- |
| `health_sample_count` | MonitorTask完成健康资源采样的次数 |
| `camera_service_stack_min_free_bytes` | CameraServiceTask启动以来的最小剩余栈空间，单位B |
| `monitor_stack_min_free_bytes` | MonitorTask启动以来的最小剩余栈空间，单位B |
| `free_heap_bytes` | 当前FreeRTOS Heap余量，单位B |
| `min_ever_free_heap_bytes` | FreeRTOS启动以来最小Heap余量，单位B |

两个任务的最小栈余量初始化为`0U`，第一次实际采样后由CMSIS-RTOS2返回的真实watermark覆盖，避免首次采样前STATUS显示`UINT32_MAX`。

## STATUS输出

现有STATUS字段、UART RX DMA区域和二进制请求统计全部保留，RTOS统计后新增：

```text
HEALTH:
  health_sample_count=
  camera_service_stack_min_free_bytes=
  monitor_stack_min_free_bytes=
  free_heap_bytes=
  min_ever_free_heap_bytes=
```

所有带`_bytes`的字段单位均为B；不输出安全阈值或PASS/FAIL结论。

## 并发说明

- CameraServiceTask只更新自己的历史最小栈余量字段。
- MonitorTask更新自身历史最小栈余量、两个Heap字段和健康采样次数。
- 每个字段使用单次32位读写，未新增Mutex、临界区或其他同步对象。
- STATUS允许读到来自不同采样时刻的字段组合，因此它是运行观测，不是严格原子、事务型快照。

## 构建结果

`cmake --build build/Debug`构建通过。

| 区域 | Stage 10 Round 1 | Stage 9结束时 | 变化 |
| --- | ---: | ---: | ---: |
| RAM | 116512 B / 192 KB | 116488 B / 192 KB | +24 B |
| CCMRAM | 0 B / 64 KB | 0 B / 64 KB | 0 B |
| FLASH | 66820 B / 1 MB | 66128 B / 1 MB | +692 B |

## 板上测试结果

四个正式工具均已完成真实板上回归：

- `tools/uart_image_request_basic.py`：PASS。
- `tools/uart_image_request_fault_test.py`：PASS。
- `tools/pc_dump_rgb565.py`：PASS。
- `tools/uart_image_request_repeat.py`：20/20 PASS。

板上`STATUS`记录的HEALTH字段如下：

```text
health_sample_count=79
camera_service_stack_min_free_bytes=7792
monitor_stack_min_free_bytes=1872
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

同次检查的UART RX DMA统计如下：

```text
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

本次板测中UART DMA无错误、无溢出、无恢复和重同步事件；Heap当前值与历史最小值一致。

## 风险与限制

- 本轮只监控，不自动保护或复位。
- 尚未启用栈溢出Hook。
- 尚未启用malloc failed Hook。
- 尚未启用IWDG。
- `osThreadGetStackSpace()`所依据的high-water mark只能反映任务启动以来的历史最深栈使用，不能提供瞬时剩余栈。
- STATUS不是原子快照。
- 32位统计值长期运行会回绕。
- 代码构建及本轮真实板上回归已验证；这些数值仅代表本次运行样本，后续长期运行仍需继续观察。

## 阶段结论

Stage 10 Round 1代码、文档和板上回归已完成，固件构建通过。四个正式工具全部PASS，repeat连续请求20/20 PASS，UART DMA无错误或溢出。运行健康统计不会改变任务栈大小、优先级、Heap大小或通信协议。

## Stage 10 Round 1.3A 安全版初始化日志精简

- 上一版日志精简板测失败：basic收到0 B，pc_dump收不到`OV56RGB5`，repeat 20/20 FAIL；`STATUS`仍能正常响应，`last_error_code=512`，对应DCMI快照超时。
- 回滚上一版六个C/头文件后，basic PASS、pc_dump PASS、repeat 20/20 PASS，确认功能基线恢复。
- 本轮原则：只关闭成功打印，不关闭任何SCCB读写，不关闭时序回读函数调用，不改变delay、返回值、错误码或DCMI/DMA流程。
- 默认关闭：OV5640 timing readback begin/end成功提示、QVGA_TESTBAR逐项寄存器值打印、full table RGB565成功提示，以及OV5640 IDH/IDL细节INFO输出。
- 默认保留：全部寄存器读写与读取循环、读取/写入失败日志、`STATUS`输出、DUMP协议、二进制请求协议和DCMI/DMA流程。
- 待板测项目：确认启动日志减少、`STATUS`完整、basic PASS、pc_dump PASS、repeat 20/20 PASS。

## Stage 10 Round 1.3B AEC与成功日志精简

- 本轮目的：关闭默认启动时的AEC/AGC寄存器Dump和重复成功日志，只保留少量关键初始化结果。
- 默认关闭：OV5640 IDH/IDL细节日志、testbar/real image成功细节、PC dump初始化成功ret、AEC/AGC寄存器Dump，以及AEC、AWB和image tuning成功ret日志。
- 默认保留：PCF8574初始化成功、OV5640 ID识别成功、`Camera init OK`、全部错误日志和完整`STATUS`输出。
- 安全原则：只关闭打印，不删除SCCB读写、寄存器读取、delay、返回值判断，不改变初始化顺序。
- 待板测项目：确认启动日志只剩少量关键行，basic PASS、pc_dump PASS、repeat 20/20 PASS，并确认`STATUS`完整。

### 板测结果

- basic测试：串口打开后`DTR=False`、`RTS=False`，收到38426 B响应，`frame_id=1`，Header与payload CRC校验一致，结果PASS。
- pc_dump测试：使用`tag=log_trim_b`，收到`Frame ID=2`的160x120图像，payload为38400 B，`CRC32=0x120470BD`，图像和报告均正常生成，结果PASS。
- repeat测试：连续请求20次，成功20次、失败0次，成功率100%；`frame_id=3`到`22`且保持连续，结果PASS。
- 默认启动日志已经精简为以下三条关键结果：

```text
[INFO][0] PCF8574 initialized successfully
[INFO][98] OV5640 OK, ID = 0x5640
[INFO][1982] Camera init OK
```

- 默认不再输出：OV5640 IDH/IDL细节日志、OV5640 timing readback begin/end、QVGA_TESTBAR逐项寄存器回读、testbar init done、real image init done、PC dump init ret=0、AEC/AGC大段Dump，以及AEC/AWB/image tuning成功ret日志。
- 结论：本轮只关闭打印，不改变SCCB读写、寄存器读取、delay、返回值判断、DUMP协议和UART DMA；板测确认功能正常。

## Stage 10 Round 2 FreeRTOS运行保护Hook

- 本轮目的：任务栈溢出时进入`vApplicationStackOverflowHook`，动态内存分配失败时进入`vApplicationMallocFailedHook`，`configASSERT`失败时进入`vAssertCalled`；`STATUS`新增Hook状态字段。
- FreeRTOS配置：`configCHECK_FOR_STACK_OVERFLOW=2`、`configUSE_MALLOC_FAILED_HOOK=1`，`configASSERT`调用`vAssertCalled`。
- Hook行为：记录`hook_fault_code`、`hook_fault_count`和`assert_line`，输出一次`FATAL`日志后关闭中断并停在死循环；不主动复位，不配置IWDG或WWDG。
- 正常运行预期：`hook_fault_code=0`、`hook_fault_count=0`、`assert_line=0`，basic PASS、pc_dump PASS、repeat 20/20 PASS。
- 后续计划：Stage 10 Round 3再增加IWDG与任务心跳；本轮不加入看门狗。

### 正常路径板测结果

- 启动日志未出现任何`FATAL`信息：未发生stack overflow、malloc failed或configASSERT。
- basic测试：PASS。
- pc_dump测试：PASS；图像成功获取，CRC校验通过，报告正常生成。
- repeat测试：20/20 PASS。
- Hook状态：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

- HEALTH实测：

```text
camera_service_stack_min_free_bytes=7792
monitor_stack_min_free_bytes=1872
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

- UART RX DMA实测：

```text
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

- 结论：Stage 10 Round 2正常路径验证通过。FreeRTOS Hook已接入，正常运行时未误触发；DUMP、二进制请求、PC Dump和UART DMA均保持正常。

## Stage 10 Round 3A 任务心跳监控框架

- 本轮目的：增加CameraServiceTask和MonitorTask心跳，并在`STATUS`中显示心跳计数、最近心跳时间及心跳年龄，为Round 3B IWDG做准备。
- 本轮不启用IWDG或WWDG，不主动复位，不做卡死判定，也不改变任务优先级、任务栈大小和Heap大小。
- 新增`STATUS`字段：`camera_service_heartbeat_count`、`monitor_heartbeat_count`、`camera_service_heartbeat_ms`、`monitor_heartbeat_ms`、`camera_service_heartbeat_age_ms`、`monitor_heartbeat_age_ms`。
- CameraServiceTask在每轮主循环更新计数和最近心跳时间；MonitorTask在每个1000 ms监控周期更新。获取状态时计算心跳年龄，若当前tick小于最近心跳tick则按0处理。
- 正常运行预期：两个`heartbeat_count`持续增加，两个`heartbeat_age_ms`保持在合理范围；basic PASS、pc_dump PASS、repeat 20/20 PASS，UART DMA无错误无溢出，`hook_fault_code`保持为0。

### 正常路径板测结果

- 启动日志：无`FATAL`、无IWDG、无WWDG，也未出现reset异常提示。
- basic测试：PASS。
- pc_dump测试：PASS；图像成功获取且CRC校验通过。图像质量报告提示存在过曝和模糊，但不影响本轮功能测试通过的结论。
- repeat测试：20/20 PASS，`frame_id`保持连续。
- HEARTBEAT实测：

```text
camera_service_heartbeat_count=648
monitor_heartbeat_count=78
camera_service_heartbeat_ms=141740
monitor_heartbeat_ms=140941
camera_service_heartbeat_age_ms=97
monitor_heartbeat_age_ms=896
```

- HOOK状态：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

- UART RX DMA实测：

```text
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

- 结论：Stage 10 Round 3A正常路径验证通过。CameraServiceTask和MonitorTask心跳字段已接入`STATUS`，两个任务心跳正常更新；当前未启用IWDG/WWDG，未主动复位，Hook未误触发，DUMP、PC Dump、二进制请求和UART DMA均保持正常。

## Stage 10 Round 3B IWDG看门狗接入

- 本轮目的：启用IWDG，由MonitorTask根据两个核心任务心跳和Hook状态统一刷新，并在`STATUS`中输出IWDG运行状态；正常路径应避免误复位。
- IWDG参数：Prescaler为256，Reload为999；按LSI约32 kHz计算，设计超时时间约8 s。CameraServiceTask心跳年龄阈值为6000 ms，MonitorTask心跳年龄阈值为3000 ms。
- 喂狗条件：Hook未触发，CameraServiceTask和MonitorTask心跳均已启动，且两个心跳年龄未超过各自阈值。运行期只由MonitorTask调用`HAL_IWDG_Refresh`。
- 不满足条件时：不主动复位，不调用`NVIC_SystemReset`；记录跳过计数、时间和原因，同一种原因只输出一次简短错误日志，随后等待IWDG硬件复位。
- 跳过原因编码：0为NONE，1为CAMERA_HEARTBEAT_NOT_STARTED，2为MONITOR_HEARTBEAT_NOT_STARTED，3为CAMERA_HEARTBEAT_TIMEOUT，4为MONITOR_HEARTBEAT_TIMEOUT，5为HOOK_FAULT。
- `STATUS`新增字段：`iwdg_enabled`、`iwdg_refresh_count`、`iwdg_refresh_skip_count`、`iwdg_last_refresh_ms`、`iwdg_last_skip_ms`、`iwdg_last_skip_reason`、`iwdg_timeout_ms`、`iwdg_camera_age_limit_ms`、`iwdg_monitor_age_limit_ms`。
- 启动时只读取并输出简洁的IWDG复位标志，不恢复完整`RESET_CAUSE`诊断日志。
- 正常运行预期：basic PASS、pc_dump PASS、repeat 20/20 PASS；`iwdg_enabled=1`，刷新计数持续增加，跳过计数和最后跳过原因为0，`hook_fault_code=0`，UART DMA无错误无溢出。

### 正常路径板测结果

- 启动情况：未出现反复复位、`FATAL`或WWDG提示，IWDG正常启用。
- 首次`STATUS`的IWDG状态：

```text
iwdg_enabled=1
iwdg_refresh_count=4
iwdg_refresh_skip_count=0
iwdg_last_refresh_ms=5987
iwdg_last_skip_ms=0
iwdg_last_skip_reason=0
iwdg_timeout_ms=8000
iwdg_camera_age_limit_ms=6000
iwdg_monitor_age_limit_ms=3000
```

- basic测试：PASS。
- pc_dump测试：PASS；图像成功获取且CRC校验通过。图像质量报告提示存在过曝和模糊，但功能测试通过。
- repeat测试：20/20 PASS，`frame_id`保持连续。
- 最终`STATUS`的IWDG状态：

```text
iwdg_enabled=1
iwdg_refresh_count=145
iwdg_refresh_skip_count=0
iwdg_last_refresh_ms=207084
iwdg_last_skip_ms=0
iwdg_last_skip_reason=0
iwdg_timeout_ms=8000
iwdg_camera_age_limit_ms=6000
iwdg_monitor_age_limit_ms=3000
```

- 最终HEARTBEAT状态：

```text
camera_service_heartbeat_count=1307
monitor_heartbeat_count=145
camera_service_heartbeat_ms=207683
monitor_heartbeat_ms=207084
camera_service_heartbeat_age_ms=93
monitor_heartbeat_age_ms=692
```

- HOOK状态：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

- UART RX DMA实测：

```text
uart_dma_event_count=28
uart_dma_rx_bytes=315
stream_buffer_write_bytes=315
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

- 结论：Stage 10 Round 3B正常路径验证通过。IWDG已启用，由MonitorTask基于CameraServiceTask和MonitorTask心跳统一刷新；正常运行期间没有跳过喂狗、没有误复位，Hook未触发，DUMP、PC Dump、二进制请求和UART DMA均保持正常。

## Stage 10 Round 3C IWDG故障路径验证

- 本轮目的：增加可控测试命令`IWDGTEST CAMERA_TIMEOUT`，模拟CameraServiceTask心跳超时，使MonitorTask停止刷新IWDG，等待IWDG硬件复位，并验证复位后系统恢复正常。
- 测试状态：`iwdg_test_mode=0`表示正常，`iwdg_test_mode=1`表示强制Camera心跳超时测试。测试标志仅保存在RAM中，不写Flash，也不跨复位保存。
- 预期复位前：`iwdg_refresh_skip_count`持续增加，`iwdg_last_skip_reason=3`；系统不调用`NVIC_SystemReset`，约8秒后由IWDG硬件复位。
- 预期复位后：启动日志显示`reset: iwdg=1`，`iwdg_test_mode`恢复为0，刷新计数重新增加，正常情况下跳过计数和最后跳过原因为0；随后验证basic PASS、pc_dump PASS、repeat 20/20 PASS。
- 安全说明：测试不写死循环，不修改正式心跳字段，不故意触发栈溢出、malloc失败或`configASSERT`；不修改协议、UART DMA、任务优先级、任务栈大小和Heap。

### 故障路径板测结果

- 触发前`STATUS`的IWDG状态：

```text
iwdg_enabled=1
iwdg_refresh_count=0
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

- 执行命令：`IWDGTEST CAMERA_TIMEOUT`。
- 命令返回：`IWDG test: CAMERA_TIMEOUT enabled, wait for hardware reset.`
- 复位前观察：日志中出现`[ERROR][17046]`，随后发生IWDG硬件复位；未调用`NVIC_SystemReset`，也未出现`FATAL`。
- 复位后启动日志显示：`reset: iwdg=1`。
- 复位后`STATUS`的IWDG状态：

```text
iwdg_enabled=1
iwdg_refresh_count=13
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

- 复位后功能回归：basic PASS；pc_dump PASS，图像质量存在过曝和模糊提示，但CRC和功能验证通过；repeat 20/20 PASS且`frame_id`连续。
- 异常检查：未出现无限复位，也未出现`FATAL`。
- 结论：Stage 10 Round 3C故障路径验证通过。`IWDGTEST CAMERA_TIMEOUT`可以可控触发停止喂狗，系统最终由IWDG硬件复位；复位后RAM测试标志清零，`iwdg_test_mode`恢复为0，系统未进入无限复位，并且DUMP、PC Dump和二进制请求均恢复正常。

## Stage 10 Round 4A 长时间稳定性测试工具

- 本轮目的：新增PC端长时间二进制图像请求测试脚本，用于验证DUMP、二进制协议、UART DMA、FreeRTOS和IWDG在长时间运行下的稳定性。
- 新增脚本：`tools/uart_image_request_stability.py`。
- 默认测试规模：`count=500`、`interval=0.2 s`、单帧响应超时10 s，预计总耗时约30 min。
- 每帧检查响应长度、OV56RGB5 Header、payload CRC、`frame_id`连续性和单帧耗时，并统计LENGTH_ERROR、MAGIC_ERROR、HEADER_ERROR、CRC_ERROR、FRAME_ID_GAP、TIMEOUT_OR_EMPTY和SERIAL_ERROR。
- 输出文件：`stability_<tag>_<timestamp>.csv`、`stability_<tag>_<timestamp>_summary.txt`，以及发生失败时保存的`stability_first_failed_response_<tag>_<timestamp>.bin`。
- 板测计划：测试前记录`STATUS`，运行500次stability测试，测试后再次记录`STATUS`并执行basic、pc_dump、repeat回归；确认IWDG没有跳过刷新、Hook保持为0、UART DMA无错误无溢出。

## Stage 10 Round 4A 长时间稳定性测试结果

### 1. 测试说明

使用新增脚本：`tools/uart_image_request_stability.py`

执行命令：

```bash
python tools/uart_image_request_stability.py --count 500 --interval 0.2 --tag stage10_round4
```

测试连续发送500次二进制图像请求，每次接收38426 B的OV56RGB5图像帧，并检查响应长度、Header、payload CRC和`frame_id`连续性，同时统计每帧耗时与错误类型。测试过程不做自动重试。

### 2. smoke 测试说明

正式长测前曾执行20次smoke测试：

```bash
python tools/uart_image_request_stability.py --count 20 --interval 0.2 --tag stage10_round4_smoke
```

该次测试因以下PC端串口错误而失败：

```text
could not open port 'COM4': PermissionError(13, '拒绝访问。')
```

确认失败原因是COM4被其他串口工具占用，测试脚本未成功打开串口，也没有向STM32发送请求。因此该结果不计入STM32固件、协议、UART DMA或IWDG稳定性失败。

### 3. 500 次长测结果

- 请求总数：500
- 成功次数：500
- 失败次数：0
- 成功率：100.00%
- 平均耗时：3466.104 ms
- 最短耗时：3430.170 ms
- 最长耗时：3486.542 ms
- 首个`frame_id`：1
- 最后`frame_id`：500
- `frame_id`连续：YES
- 最终结果：PASS

### 4. 失败分类

```text
LENGTH_ERROR=0
MAGIC_ERROR=0
HEADER_ERROR=0
CRC_ERROR=0
FRAME_ID_GAP=0
TIMEOUT_OR_EMPTY=0
SERIAL_ERROR=0
```

### 5. 输出文件

- CSV路径：`captures\stability_stage10_round4_20260804_172829.csv`
- summary路径：`captures\stability_stage10_round4_20260804_172829_summary.txt`
- 本次500次长测没有发生图像请求失败，因此没有生成有效的失败响应文件。

### 6. 测试后 STATUS 关键结果

RTOS：

```text
dump_request_count=500
dump_success_count=500
dump_error_count=0
binary_request_count=500
binary_request_success_count=500
binary_request_error_count=0
last_binary_request_seq=500
last_error_code=0
last_dump_time_ms=3516
```

HEALTH：

```text
health_sample_count=3591
camera_service_stack_min_free_bytes=7816
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

HOOK：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

HEARTBEAT：

```text
camera_service_heartbeat_count=32511
monitor_heartbeat_count=3591
camera_service_heartbeat_age_ms=57
monitor_heartbeat_age_ms=656
```

IWDG：

```text
iwdg_enabled=1
iwdg_refresh_count=3591
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_timeout_ms=8000
iwdg_camera_age_limit_ms=6000
iwdg_monitor_age_limit_ms=3000
iwdg_test_mode=0
```

UART RX DMA：

```text
uart_dma_event_count=610
uart_dma_rx_bytes=7008
stream_buffer_write_bytes=7008
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 7. 结论

Stage 10 Round 4A长时间稳定性测试通过。

连续500次二进制图像请求全部成功，`frame_id`从1到500连续，未出现长度错误、Header错误、CRC错误、`frame_id`跳变、超时或串口通信错误。

测试后`STATUS`显示FreeRTOS栈余量正常；Heap当前值和历史最小值一致，无明显内存泄漏；Hook未触发；IWDG正常喂狗，未出现跳过喂狗；UART DMA无错误、无溢出、无恢复、无重同步。DUMP、二进制协议、UART DMA、FreeRTOS心跳与IWDG在约30分钟连续请求场景下保持稳定。
