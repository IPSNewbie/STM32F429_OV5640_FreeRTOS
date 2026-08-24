# Stage 10 运行稳定性与故障保护最终报告

## 1. 阶段目标

Stage 10用于完善系统运行稳定性与故障保护能力，主要目标包括：

- 增加运行状态监控。
- 增加FreeRTOS Hook保护。
- 增加任务心跳。
- 接入IWDG独立看门狗。
- 验证IWDG正常路径和故障路径。
- 进行500次长时间图像请求稳定性测试。
- 证明DUMP、二进制请求、UART DMA、FreeRTOS心跳和IWDG可以稳定协同工作。

## 2. Stage 10 Round 1：运行健康监控

本轮完成了`STATUS`中的HEALTH状态输出，记录CameraServiceTask和MonitorTask启动以来的栈最小剩余空间、当前Heap以及历史最小Heap。

最终长测后的数据：

```text
health_sample_count=3591
camera_service_stack_min_free_bytes=7816
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

结论：两个任务的栈余量正常；Heap当前值与历史最小值一致，未观察到持续下降；长时间运行中未发现明显内存泄漏迹象。

## 3. Stage 10 Round 2：FreeRTOS Hook 保护

本轮完成了以下FreeRTOS故障保护：

- `configCHECK_FOR_STACK_OVERFLOW=2`
- `configUSE_MALLOC_FAILED_HOOK=1`
- `configASSERT`
- 栈溢出Hook
- malloc失败Hook
- assert失败Hook

最终长测后的数据：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

结论：长时间运行中Hook未误触发，当前正常路径没有出现栈溢出、malloc失败或assert故障。

## 4. Stage 10 Round 3A：任务心跳监控

本轮增加了CameraServiceTask心跳、MonitorTask心跳，并在`STATUS`中增加HEARTBEAT区域。

最终长测后的数据：

```text
camera_service_heartbeat_count=32511
monitor_heartbeat_count=3591
camera_service_heartbeat_age_ms=57
monitor_heartbeat_age_ms=656
```

结论：两个任务心跳均持续更新，长测结束时任务心跳年龄正常，CameraServiceTask和MonitorTask未出现卡死迹象。

## 5. Stage 10 Round 3B：IWDG 正常路径

本轮启用IWDG，配置`Prescaler=256`、`Reload=999`，设计超时时间约8000 ms。仅由MonitorTask根据任务心跳和Hook状态统一喂狗，不在中断、DUMP路径或CameraServiceTask中喂狗。

最终长测后的数据：

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

结论：IWDG正常启用；长时间运行中没有跳过喂狗，也没有发生误复位；IWDG与连续DUMP请求兼容。

## 6. Stage 10 Round 3C：IWDG 故障路径

本轮新增`IWDGTEST CAMERA_TIMEOUT`命令，用于模拟CameraServiceTask心跳超时。命令触发后MonitorTask停止喂狗，不调用`NVIC_SystemReset`，等待IWDG硬件复位；测试状态仅保存在RAM中，复位后自动清零。

板测结果：

- 触发前`iwdg_test_mode=0`。
- 执行`IWDGTEST CAMERA_TIMEOUT`后返回：`IWDG test: CAMERA_TIMEOUT enabled, wait for hardware reset.`
- 日志中出现`ERROR`后发生复位。
- 复位后启动日志显示`reset: iwdg=1`。
- 复位后`iwdg_test_mode=0`。
- 复位后basic PASS。
- 复位后pc_dump PASS。
- 复位后repeat 20/20 PASS。
- 未出现无限复位。
- 未出现`FATAL`。

结论：IWDG故障路径验证通过。系统可以在任务心跳异常时停止喂狗，并由硬件看门狗复位恢复。

## 7. Stage 10 Round 4A：500 次长时间稳定性测试

测试命令：

```bash
python tools/uart_image_request_stability.py --count 500 --interval 0.2 --tag stage10_round4
```

测试结果：

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

失败分类：

```text
LENGTH_ERROR=0
MAGIC_ERROR=0
HEADER_ERROR=0
CRC_ERROR=0
FRAME_ID_GAP=0
TIMEOUT_OR_EMPTY=0
SERIAL_ERROR=0
```

输出文件：

- `captures\stability_stage10_round4_20260804_172829.csv`
- `captures\stability_stage10_round4_20260804_172829_summary.txt`

补充说明：20次smoke测试曾因COM4被占用失败，报错为`PermissionError(13, '拒绝访问。')`。该错误发生在PC端串口打开阶段，没有成功向STM32发送请求，因此不计入固件稳定性失败。

## 8. 最终 STATUS 汇总

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

## 9. Stage 10 总结

Stage 10已完成运行稳定性与故障保护建设。当前系统具备运行健康监控、FreeRTOS Hook保护、任务心跳、IWDG看门狗保护、故障路径复位验证和长时间图像请求稳定性验证能力。

经过500次连续二进制图像请求测试，系统未出现协议错误、CRC错误、`frame_id`跳变、UART DMA溢出、任务心跳异常、Hook触发或IWDG误复位。说明当前FreeRTOS多任务架构、UART DMA接收、二进制请求解析、OV56RGB5图像发送和IWDG喂狗策略在当前测试条件下稳定。

## 10. 后续建议

1. `IWDGTEST CAMERA_TIMEOUT`属于调试命令，后续发布版本可以用宏开关关闭，避免现场误触发。
2. 当前图像传输速率受115200串口限制，单帧约3.46 s，后续可考虑更高波特率或USB/SDIO等更高速链路。
3. 后续若进入SD卡拍照功能，需要注意SDIO与DCMI引脚冲突，不能直接假设采集和SDIO可并行工作。
4. 当前Stage 10可以作为简历项目中“稳定性、故障保护、协议测试、长时间压测”的核心支撑材料。
