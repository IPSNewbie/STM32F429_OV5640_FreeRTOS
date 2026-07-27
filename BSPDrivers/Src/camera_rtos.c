#include "camera_rtos.h"

#include "camera_cli.h"
#include "camera_dcmi_dma.h"
#include "camera_frame_buffer.h"
#include "camera_image_process.h"
#include "camera_pc_dump.h"
#include "cmsis_os.h"
#include "uart_rx_dma.h"

#include <stddef.h>

//============================================================================
// @file    camera_rtos.c
// @brief   摄像头 RTOS 任务模块实现
//          提供 CameraServiceTask（处理 PC 命令、拍照、图像处理、发送帧）
//          和 MonitorTask（定期统计监控）两个 FreeRTOS 任务。
//============================================================================

// 等待 DCMI 快照完成的最大超时时间（毫秒）
#define CAMERA_RTOS_SNAPSHOT_TIMEOUT_MS          3000U

// CameraServiceTask 每次从 StreamBuffer 读取的最大字节数
#define CAMERA_RTOS_UART_RX_CHUNK_SIZE             32U

// CameraServiceTask 等待接收字节的最长时间
#define CAMERA_RTOS_UART_READ_TIMEOUT_MS           100U

// 静态变量：保存 UART 句柄（用于日志和 PC 通信）
static UART_HandleTypeDef *s_camera_rtos_uart;

// 静态变量：运行统计信息结构体
static CameraRtosStats_t s_camera_rtos_stats;

// 静态变量：帧序号（每次成功发送后递增）
static uint32_t s_camera_rtos_frame_id = 1U;

// 重置所有统计计数为 0
static void Camera_RTOS_ClearStats(void)
{
    s_camera_rtos_stats.camera_service_loop_count = 0U;
    s_camera_rtos_stats.monitor_tick_count = 0U;
    s_camera_rtos_stats.cli_command_count = 0U;
    s_camera_rtos_stats.cli_unknown_count = 0U;
    s_camera_rtos_stats.dump_request_count = 0U;
    s_camera_rtos_stats.dump_success_count = 0U;
    s_camera_rtos_stats.dump_error_count = 0U;
    s_camera_rtos_stats.uart_none_count = 0U;
    s_camera_rtos_stats.uart_pending_count = 0U;
    s_camera_rtos_stats.uart_error_count = 0U;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_NONE;
    s_camera_rtos_stats.last_dump_time_ms = 0U;
    s_camera_rtos_stats.last_status_time_ms = 0U;
    s_camera_rtos_stats.uptime_ms = 0U;
}

// 记录一条完整 CLI 命令
void Camera_RTOS_RecordCliCommand(void)
{
    s_camera_rtos_stats.cli_command_count++;
}

// 记录一条未知 CLI 命令
void Camera_RTOS_RecordCliUnknown(void)
{
    s_camera_rtos_stats.cli_unknown_count++;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UNKNOWN_CMD;
}

// 记录一次 DUMP 请求
void Camera_RTOS_RecordDumpRequest(void)
{
    s_camera_rtos_stats.dump_request_count++;
}

// 记录一次成功 DUMP 及其耗时
void Camera_RTOS_RecordDumpSuccess(uint32_t elapsed_ms)
{
    s_camera_rtos_stats.dump_success_count++;
    s_camera_rtos_stats.last_dump_time_ms = elapsed_ms;
}

// 记录一次失败 DUMP 及其错误码
void Camera_RTOS_RecordDumpError(uint32_t error_code)
{
    s_camera_rtos_stats.dump_error_count++;
    s_camera_rtos_stats.last_error_code =
        (error_code == CAMERA_RTOS_ERR_NONE) ? CAMERA_RTOS_ERR_DUMP_FAILED : error_code;
}

// 记录一次 UART 无数据状态
void Camera_RTOS_RecordUartNone(void)
{
    s_camera_rtos_stats.uart_none_count++;
}

// 记录一次 UART 命令接收中状态
void Camera_RTOS_RecordUartPending(void)
{
    s_camera_rtos_stats.uart_pending_count++;
}

// 记录一次 UART 接收错误
void Camera_RTOS_RecordUartError(void)
{
    s_camera_rtos_stats.uart_error_count++;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
}

// 记录最近一次 STATUS 命令执行时间
void Camera_RTOS_RecordStatus(uint32_t time_ms)
{
    s_camera_rtos_stats.last_status_time_ms = time_ms;
}

// 将 UART DMA 错误统计同步到 Stage 8 兼容字段
static void Camera_RTOS_SyncUartErrorCount(void)
{
    const UartRxDmaStats_t *uart_stats = UART_RxDma_GetStats();

    if (uart_stats != NULL)
    {
        s_camera_rtos_stats.uart_error_count = uart_stats->uart_error_count;
    }
}

// 在任务上下文处理 UART DMA 错误恢复，并丢弃恢复前的残缺输入
static uint8_t Camera_RTOS_RecoverUartInputIfNeeded(void)
{
    UartRxDmaRecoveryResult_t recovery_result;

    recovery_result = UART_RxDma_RecoverIfNeeded();
    Camera_RTOS_SyncUartErrorCount();

    if (recovery_result == UART_RX_DMA_RECOVERY_NONE)
    {
        return 0U;
    }

    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_DMA_RECOVERY;
    if (recovery_result == UART_RX_DMA_RECOVERY_RETRY)
    {
        (void)osDelay(1U);
        return 1U;
    }

    Camera_PC_Dump_ResetCommandParser();
    (void)UART_RxDma_Drain();
    if (UART_RxDma_HasOverflow() != 0U)
    {
        UART_RxDma_ClearOverflow();
    }
    return 1U;
}

// 检测 StreamBuffer 溢出，清除残缺文本状态并排空现有字节
static uint8_t Camera_RTOS_DiscardOverflowedInput(void)
{
    if (UART_RxDma_HasOverflow() == 0U)
    {
        return 0U;
    }

    Camera_PC_Dump_ResetCommandParser();
    (void)UART_RxDma_Drain();
    UART_RxDma_ClearOverflow();
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_STREAM_OVERFLOW;
    return 1U;
}

// 执行完整的“捕获 → 图像处理 → 发送”流程，返回错误码（0 表示成功）
static uint32_t Camera_RTOS_CaptureProcessAndSend(void)
{
    uint8_t snapshot_ret;
    uint8_t dump_ret;
    uint32_t snapshot_start_tick;
    CameraFrameBufferStatus_t commit_ret;
    CameraImageProcessStatus_t process_ret;
    CameraProcessMode_t process_mode;
    uint8_t binary_threshold;

    // 清除快照完成标志
    Camera_DCMI_ClearSnapshotDone();

    // 启动 DCMI 快照，将图像数据写入 PC Dump 缓冲区（即后台缓冲区）
    snapshot_ret = Camera_DCMI_StartSnapshotToBuffer(
        Camera_PC_Dump_GetBufferAddress(),
        Camera_PC_Dump_GetWordCount());
    if (snapshot_ret != 0U)
    {
        Camera_DCMI_Stop();
        // 返回错误码：基础码 + 子错误码
        return CAMERA_RTOS_ERR_SNAPSHOT_START_BASE | (uint32_t)snapshot_ret;
    }

    // 等待快照完成，超时则停止并返回错误
    snapshot_start_tick = HAL_GetTick();
    while (Camera_DCMI_IsSnapshotDone() == 0U)
    {
        if ((HAL_GetTick() - snapshot_start_tick) > CAMERA_RTOS_SNAPSHOT_TIMEOUT_MS)
        {
            Camera_DCMI_Stop();
            return CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT;
        }
        (void)osDelay(1U);  // 让出 CPU，避免忙等
    }

    Camera_DCMI_Stop();

    // 提交后台缓冲区，使新捕获的帧成为前台帧
    commit_ret = Camera_FrameBuffer_CommitBackBuffer();
    if (commit_ret != CAMERA_FB_OK)
    {
        return CAMERA_RTOS_ERR_CAPTURE_COMMIT_BASE | (uint32_t)commit_ret;
    }

    // 获取当前 CLI 配置的处理模式和二值化阈值
    process_mode = Camera_CLI_GetProcessMode();
    binary_threshold = Camera_CLI_GetBinaryThreshold();

    // 对帧缓冲区应用图像处理（旁路/灰度/二值化）
    process_ret = Camera_ImageProcess_ApplyToFrameBuffer(process_mode, binary_threshold);
    if (process_ret != CAMERA_PROCESS_OK)
    {
        // 如果处理失败，尝试回退到旁路模式（确保图像可发送）
        process_ret = Camera_ImageProcess_ApplyToFrameBuffer(
            CAMERA_PROCESS_MODE_BYPASS,
            binary_threshold);
        if (process_ret != CAMERA_PROCESS_OK)
        {
            return CAMERA_RTOS_ERR_IMAGE_PROCESS_BASE | (uint32_t)process_ret;
        }
    }

    // 将当前前台帧通过 UART 发送给 PC
    dump_ret = Camera_PC_Dump_SendFrame(s_camera_rtos_uart, s_camera_rtos_frame_id);
    if (dump_ret != 0U)
    {
        return CAMERA_RTOS_ERR_DUMP_SEND_BASE | (uint32_t)dump_ret;
    }

    // 成功发送后递增帧序号
    s_camera_rtos_frame_id++;
    return CAMERA_RTOS_ERR_NONE;
}

// 初始化摄像头 RTOS 模块（保存 UART 句柄，重置统计和帧号）
void Camera_RTOS_Init(UART_HandleTypeDef *huart)
{
    s_camera_rtos_uart = NULL;
    s_camera_rtos_frame_id = 1U;
    Camera_RTOS_ClearStats();

    if (huart == NULL)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_NULL;
        return;
    }

    s_camera_rtos_uart = huart;
}

// 摄像头服务任务（FreeRTOS 任务主循环）
void Camera_RTOS_CameraServiceTask(void *argument)
{
    uint8_t rx_chunk[CAMERA_RTOS_UART_RX_CHUNK_SIZE];
    uint8_t command;
    size_t received;
    size_t i;
    uint32_t error_code;
    uint32_t dump_start_tick;
    uint32_t dump_elapsed_ms;

    (void)argument;  // 未使用的参数

    Camera_PC_Dump_ResetCommandParser();

    // 仅在任务启动阶段初始化，正常运行时不重复启动 DMA
    while (s_camera_rtos_uart == NULL)
    {
        s_camera_rtos_stats.camera_service_loop_count++;
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_NULL;
        (void)osDelay(1000U);
    }

    while (UART_RxDma_Init(s_camera_rtos_uart) != HAL_OK)
    {
        s_camera_rtos_stats.camera_service_loop_count++;
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_DMA_INIT;
        (void)osDelay(1000U);
    }

    for (;;)
    {
        s_camera_rtos_stats.camera_service_loop_count++;

        if (Camera_RTOS_RecoverUartInputIfNeeded() != 0U)
        {
            continue;
        }

        if (Camera_RTOS_DiscardOverflowedInput() != 0U)
        {
            continue;
        }

        received = UART_RxDma_Read(rx_chunk,
                                   sizeof(rx_chunk),
                                   CAMERA_RTOS_UART_READ_TIMEOUT_MS);
        if (received == 0U)
        {
            // 先排除等待期间出现的错误或溢出，再记录纯读取超时
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                continue;
            }
            Camera_RTOS_RecordUartNone();
            continue;
        }

        for (i = 0U; i < received; ++i)
        {
            // 错误或溢出发生后放弃当前读取块的剩余字节
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                break;
            }

            command = Camera_PC_Dump_FeedCommandByte(s_camera_rtos_uart,
                                                     rx_chunk[i]);
            if (command == CAMERA_PC_DUMP_CMD_PENDING)
            {
                Camera_RTOS_RecordUartPending();
                continue;
            }

            // CLI 命令已在文本行解析器内部处理
            if (command == CAMERA_PC_DUMP_CMD_CLI)
            {
                continue;
            }

            if (command != CAMERA_PC_DUMP_CMD_DUMP)
            {
                s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
                continue;
            }

            // DUMP 仍在 CameraServiceTask 中串行执行，且发送前不输出文本
            Camera_RTOS_RecordDumpRequest();
            dump_start_tick = HAL_GetTick();
            error_code = Camera_RTOS_CaptureProcessAndSend();
            dump_elapsed_ms = HAL_GetTick() - dump_start_tick;
            if (error_code == CAMERA_RTOS_ERR_NONE)
            {
                Camera_RTOS_RecordDumpSuccess(dump_elapsed_ms);
            }
            else
            {
                s_camera_rtos_stats.last_dump_time_ms = dump_elapsed_ms;
                Camera_RTOS_RecordDumpError(error_code);
            }
        }
    }
}

// MonitorTask：每 1000 ms 更新心跳计数，不使用 UART。
void Camera_RTOS_MonitorTask(void *argument)
{
    (void)argument;

    for (;;)
    {
        (void)osDelay(1000U);
        s_camera_rtos_stats.monitor_tick_count++;
        s_camera_rtos_stats.uptime_ms += 1000U;
    }
}

// 获取运行统计信息的只读指针
const CameraRtosStats_t *Camera_RTOS_GetStats(void)
{
    return &s_camera_rtos_stats;
}
