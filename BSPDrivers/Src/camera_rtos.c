#include "camera_rtos.h"

#include "camera_cli.h"
#include "camera_dcmi_dma.h"
#include "camera_frame_buffer.h"
#include "camera_image_process.h"
#include "camera_pc_dump.h"
#include "camera_uart_dispatcher.h"
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

// CameraServiceTask 栈余量的周期采样间隔（毫秒）
#define CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS         1000U

// 静态变量：保存 UART 句柄（用于日志和 PC 通信）
static UART_HandleTypeDef *s_camera_rtos_uart;

// 静态变量：运行统计信息结构体
static CameraRtosStats_t s_camera_rtos_stats;

// 静态变量：文本与二进制协议分发器
static CameraUartDispatcher_t s_camera_uart_dispatcher;

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
    s_camera_rtos_stats.binary_request_count = 0U;
    s_camera_rtos_stats.binary_request_success_count = 0U;
    s_camera_rtos_stats.binary_request_error_count = 0U;
    s_camera_rtos_stats.binary_request_crc_error_count = 0U;
    s_camera_rtos_stats.binary_request_version_error_count = 0U;
    s_camera_rtos_stats.binary_request_type_error_count = 0U;
    s_camera_rtos_stats.binary_request_length_error_count = 0U;
    s_camera_rtos_stats.binary_request_eof_error_count = 0U;
    s_camera_rtos_stats.binary_request_timeout_count = 0U;
    s_camera_rtos_stats.last_binary_request_seq = 0U;
    s_camera_rtos_stats.last_binary_error_code = IMAGE_REQUEST_PARSE_NONE;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_NONE;
    s_camera_rtos_stats.last_dump_time_ms = 0U;
    s_camera_rtos_stats.last_status_time_ms = 0U;
    s_camera_rtos_stats.uptime_ms = 0U;
    s_camera_rtos_stats.health_sample_count = 0U;
    s_camera_rtos_stats.camera_service_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.monitor_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.free_heap_bytes = 0U;
    s_camera_rtos_stats.min_ever_free_heap_bytes = 0U;
    s_camera_rtos_stats.hook_fault_code = 0U;
    s_camera_rtos_stats.hook_fault_count = 0U;
    s_camera_rtos_stats.assert_line = 0U;
}

// CMSIS-RTOS2 返回当前任务启动以来的历史最小栈余量，单位为字节
static uint32_t Camera_RTOS_GetCurrentTaskStackMinFreeBytes(void)
{
    return osThreadGetStackSpace(osThreadGetId());
}

// 在 CameraServiceTask 自身上下文读取最新的历史最小栈余量
static void Camera_RTOS_UpdateCameraServiceStackStats(void)
{
    s_camera_rtos_stats.camera_service_stack_min_free_bytes =
        Camera_RTOS_GetCurrentTaskStackMinFreeBytes();
}

// 在 MonitorTask 自身上下文更新历史最小栈余量、Heap 和健康采样计数
static void Camera_RTOS_UpdateMonitorHealthStats(void)
{
    s_camera_rtos_stats.monitor_stack_min_free_bytes =
        Camera_RTOS_GetCurrentTaskStackMinFreeBytes();
    s_camera_rtos_stats.free_heap_bytes = (uint32_t)xPortGetFreeHeapSize();
    s_camera_rtos_stats.min_ever_free_heap_bytes =
        (uint32_t)xPortGetMinimumEverFreeHeapSize();
    s_camera_rtos_stats.health_sample_count++;
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

// 记录FreeRTOS严重错误Hook；该函数不分配内存，也不执行复杂业务
void Camera_RTOS_RecordHookFault(uint32_t fault_code, uint32_t assert_line)
{
    s_camera_rtos_stats.hook_fault_code = fault_code;
    s_camera_rtos_stats.hook_fault_count++;
    s_camera_rtos_stats.assert_line = assert_line;
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

    CameraUartDispatcher_Reset(&s_camera_uart_dispatcher);
    Camera_PC_Dump_ResetCommandParser();
    (void)UART_RxDma_Drain();
    if (UART_RxDma_HasOverflow() != 0U)
    {
        UART_RxDma_ClearOverflow();
    }
    return 1U;
}

// 检测 StreamBuffer 溢出，清除残缺协议状态并排空现有字节
static uint8_t Camera_RTOS_DiscardOverflowedInput(void)
{
    if (UART_RxDma_HasOverflow() == 0U)
    {
        return 0U;
    }

    CameraUartDispatcher_Reset(&s_camera_uart_dispatcher);
    Camera_PC_Dump_ResetCommandParser();
    (void)UART_RxDma_Drain();
    UART_RxDma_ClearOverflow();
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_STREAM_OVERFLOW;
    return 1U;
}

// 执行完整的“捕获 → 图像处理 → 发送”流程，返回错误码（0 表示成功）
static uint32_t Camera_RTOS_CaptureProcessAndSend(void)
{
    uint8_t snapshot_ret;                      // DCMI 快照启动返回值（0 成功）
    uint8_t dump_ret;                          // PC Dump 发送返回值（0 成功）
    uint32_t snapshot_start_tick;              // 快照启动时的时间戳（毫秒）
    CameraFrameBufferStatus_t commit_ret;      // 帧缓冲区提交结果
    CameraImageProcessStatus_t process_ret;    // 图像处理结果
    CameraProcessMode_t process_mode;          // 当前 CLI 配置的处理模式
    uint8_t binary_threshold;                  // 当前 CLI 配置的二值化阈值

    // 清除之前可能残留的快照完成标志，确保本次快照状态干净
    Camera_DCMI_ClearSnapshotDone();

    // 启动 DCMI 快照，将图像数据写入 PC Dump 缓冲区（即后台缓冲区）
    // 该缓冲区地址和传输字数是固定的，由 PC Dump 模块提供
    snapshot_ret = Camera_DCMI_StartSnapshotToBuffer(
        Camera_PC_Dump_GetBufferAddress(),
        Camera_PC_Dump_GetWordCount());
    if (snapshot_ret != 0U)
    {
        Camera_DCMI_Stop();   // 启动失败则立即停止 DCMI，避免异常状态
        // 返回组合错误码：基础错误码 + DCMI 返回的子错误码
        return CAMERA_RTOS_ERR_SNAPSHOT_START_BASE | (uint32_t)snapshot_ret;
    }

    // 等待快照完成，超时则停止并返回错误
    snapshot_start_tick = HAL_GetTick();
    while (Camera_DCMI_IsSnapshotDone() == 0U)
    {
        // 检查是否超过设定的超时时间（通常 3000ms）
        if ((HAL_GetTick() - snapshot_start_tick) > CAMERA_RTOS_SNAPSHOT_TIMEOUT_MS)
        {
            Camera_DCMI_Stop();   // 超时强制停止 DCMI
            return CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT;
        }
        (void)osDelay(1U);  // 让出 CPU，避免忙等，同时保证响应及时
    }

    Camera_DCMI_Stop();   // 快照完成，正常停止 DCMI（释放资源）

    // 提交后台缓冲区，使新捕获的帧成为前台帧（双缓冲切换）
    commit_ret = Camera_FrameBuffer_CommitBackBuffer();
    if (commit_ret != CAMERA_FB_OK)
    {
        // 提交失败，返回包含帧缓冲区错误码的组合错误
        return CAMERA_RTOS_ERR_CAPTURE_COMMIT_BASE | (uint32_t)commit_ret;
    }

    // 获取当前 CLI 配置的处理模式和二值化阈值（可能通过串口命令动态修改）
    process_mode = Camera_CLI_GetProcessMode();
    binary_threshold = Camera_CLI_GetBinaryThreshold();

    // 对帧缓冲区应用图像处理（旁路/灰度/二值化），处理结果直接写回后台缓冲区，
    // 然后自动提交成为新的前台帧（ApplyToFrameBuffer 内部会提交）
    process_ret = Camera_ImageProcess_ApplyToFrameBuffer(process_mode, binary_threshold);
    if (process_ret != CAMERA_PROCESS_OK)
    {
        // 如果用户配置的处理模式执行失败（例如内存不足或参数异常），
        // 则尝试回退到旁路模式（不处理图像），确保至少能发送原始图像数据
        process_ret = Camera_ImageProcess_ApplyToFrameBuffer(
            CAMERA_PROCESS_MODE_BYPASS,
            binary_threshold);
        if (process_ret != CAMERA_PROCESS_OK)
        {
            // 旁路模式仍然失败，说明系统存在严重错误，返回图像处理错误码
            return CAMERA_RTOS_ERR_IMAGE_PROCESS_BASE | (uint32_t)process_ret;
        }
        // 注意：若旁路模式成功，则不会返回错误，继续执行发送流程
    }

    // 将当前前台帧（已经过处理或旁路）通过 UART 打包发送给 PC
    // 帧序号 s_camera_rtos_frame_id 会随每次成功发送递增，便于 PC 端识别
    dump_ret = Camera_PC_Dump_SendFrame(s_camera_rtos_uart, s_camera_rtos_frame_id);
    if (dump_ret != 0U)
    {
        // 发送失败（可能因为 UART 错误、帧头/CRC 错误等），返回对应的发送错误码
        return CAMERA_RTOS_ERR_DUMP_SEND_BASE | (uint32_t)dump_ret;
    }

    // 成功发送后递增帧序号，为下一帧准备
    s_camera_rtos_frame_id++;
    return CAMERA_RTOS_ERR_NONE;   // 全部流程顺利完成，返回无错误标志
}

// 文本和二进制请求共用同一条 DUMP 执行路径
static uint8_t Camera_RTOS_ProcessDumpRequest(void)
{
    uint32_t error_code;
    uint32_t dump_start_tick;
    uint32_t dump_elapsed_ms;

    Camera_RTOS_RecordDumpRequest();
    dump_start_tick = HAL_GetTick();
    error_code = Camera_RTOS_CaptureProcessAndSend();
    dump_elapsed_ms = HAL_GetTick() - dump_start_tick;
    Camera_RTOS_UpdateCameraServiceStackStats();
    if (error_code == CAMERA_RTOS_ERR_NONE)
    {
        Camera_RTOS_RecordDumpSuccess(dump_elapsed_ms);
        return 1U;
    }

    s_camera_rtos_stats.last_dump_time_ms = dump_elapsed_ms;
    Camera_RTOS_RecordDumpError(error_code);
    return 0U;
}

// 记录合法二进制请求，序号仅用于请求统计
static void Camera_RTOS_RecordBinaryRequest(
    const ImageRequestFrame_t *request)
{
    s_camera_rtos_stats.binary_request_count++;
    s_camera_rtos_stats.last_binary_request_seq = request->seq;
}

// 记录二进制解析错误并更新分类计数
static void Camera_RTOS_RecordBinaryError(
    ImageRequestParseResult_t parse_result)
{
    s_camera_rtos_stats.binary_request_error_count++;
    s_camera_rtos_stats.last_binary_error_code = (uint32_t)parse_result;

    switch (parse_result)
    {
        case IMAGE_REQUEST_PARSE_CRC_ERROR:
            s_camera_rtos_stats.binary_request_crc_error_count++;
            break;

        case IMAGE_REQUEST_PARSE_VERSION_ERROR:
            s_camera_rtos_stats.binary_request_version_error_count++;
            break;

        case IMAGE_REQUEST_PARSE_TYPE_ERROR:
            s_camera_rtos_stats.binary_request_type_error_count++;
            break;

        case IMAGE_REQUEST_PARSE_LENGTH_ERROR:
            s_camera_rtos_stats.binary_request_length_error_count++;
            break;

        case IMAGE_REQUEST_PARSE_EOF_ERROR:
            s_camera_rtos_stats.binary_request_eof_error_count++;
            break;

        default:
            break;
    }
}

// 记录二进制半帧超时，不发送任何文本响应
static void Camera_RTOS_RecordBinaryTimeout(void)
{
    s_camera_rtos_stats.binary_request_timeout_count++;
    s_camera_rtos_stats.last_binary_error_code = IMAGE_REQUEST_PARSE_TIMEOUT;
}

// 将一个文本字节送入现有命令解析器
static void Camera_RTOS_ProcessTextByte(uint8_t byte)
{
    uint8_t command;

    command = Camera_PC_Dump_FeedCommandByte(s_camera_rtos_uart, byte);
    if (command == CAMERA_PC_DUMP_CMD_PENDING)
    {
        Camera_RTOS_RecordUartPending();
        return;
    }

    // CLI 命令已在文本行解析器内部处理
    if (command == CAMERA_PC_DUMP_CMD_CLI)
    {
        return;
    }

    if (command == CAMERA_PC_DUMP_CMD_DUMP)
    {
        (void)Camera_RTOS_ProcessDumpRequest();
        return;
    }

    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
}

// 处理分发器产生的单个业务事件
static void Camera_RTOS_ProcessDispatchEvent(
    CameraUartDispatchResult_t result,
    const CameraUartDispatchEvent_t *event)
{
    switch (result)
    {
        case CAMERA_UART_DISPATCH_TEXT_BYTE:
            Camera_RTOS_ProcessTextByte(event->text_byte);
            break;

        case CAMERA_UART_DISPATCH_IMAGE_REQUEST:
            Camera_RTOS_RecordBinaryRequest(&event->image_request);
            if (Camera_RTOS_ProcessDumpRequest() != 0U)
            {
                s_camera_rtos_stats.binary_request_success_count++;
            }
            break;

        case CAMERA_UART_DISPATCH_BINARY_ERROR:
            Camera_RTOS_RecordBinaryError(event->binary_result);
            break;

        case CAMERA_UART_DISPATCH_BINARY_TIMEOUT:
            Camera_RTOS_RecordBinaryTimeout();
            break;

        case CAMERA_UART_DISPATCH_BAD_ARGUMENT:
            s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
            break;

        case CAMERA_UART_DISPATCH_NONE:
        default:
            break;
    }
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
// 功能：管理 UART 命令接收、DMA 数据传输、命令解析及图像采集触发
void Camera_RTOS_CameraServiceTask(void *argument)
{
    uint8_t rx_chunk[CAMERA_RTOS_UART_RX_CHUNK_SIZE];    // 用于从 DMA 环形缓冲区读取数据的临时块
    size_t received;                                     // 本次实际读取的字节数
    size_t i;                                            // 循环索引
    uint32_t stack_sample_tick;                          // 上次采样栈使用量的时间戳
    uint32_t current_tick;                               // 当前时间戳
    CameraUartDispatchEvent_t dispatch_event;            // 解析器产生的事件（如收到完整命令）
    CameraUartDispatchResult_t dispatch_result;          // 解析器处理单字节的返回结果

    (void)argument;  // 未使用的参数，避免编译警告

    // 初始化 UART 命令分发器（负责解析字节流、识别命令帧）
    CameraUartDispatcher_Init(&s_camera_uart_dispatcher);
    // 重置 PC Dump 命令解析器的内部状态（清空缓存行）
    Camera_PC_Dump_ResetCommandParser();

    // 等待 UART 句柄被外部初始化（Camera_RTOS_Init 设置）
    // 若句柄为空，则记录错误并延时重试
    while (s_camera_rtos_uart == NULL)
    {
        s_camera_rtos_stats.camera_service_loop_count++;
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_NULL;
        (void)osDelay(1000U);
    }

    // 初始化 UART DMA 接收（配置 DMA 环形缓冲区，启动连续接收）
    // 若初始化失败，则记录错误并反复重试
    while (UART_RxDma_Init(s_camera_rtos_uart) != HAL_OK)
    {
        s_camera_rtos_stats.camera_service_loop_count++;
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_DMA_INIT;
        (void)osDelay(1000U);
    }

    // 首次采样并更新任务栈使用统计（用于监控堆栈溢出）
    Camera_RTOS_UpdateCameraServiceStackStats();
    stack_sample_tick = HAL_GetTick();

    // 主循环：反复从 DMA 环形缓冲区读取数据，送入解析器处理
    for (;;)
    {
        // 任务运行计数累加（供统计）
        s_camera_rtos_stats.camera_service_loop_count++;

        // 检测并恢复 UART 输入错误（如帧错误、噪声等）
        if (Camera_RTOS_RecoverUartInputIfNeeded() != 0U)
        {
            continue;  // 若恢复过程中发现问题，重新开始循环
        }

        // 检测并丢弃因缓冲区溢出丢失的输入（避免解析器状态混乱）
        if (Camera_RTOS_DiscardOverflowedInput() != 0U)
        {
            continue;
        }

        // 从 DMA 环形缓冲区读取一块数据（非阻塞，带超时）
        received = UART_RxDma_Read(rx_chunk,
                                   sizeof(rx_chunk),
                                   CAMERA_RTOS_UART_READ_TIMEOUT_MS);
        if (received == 0U)
        {
            // 读取超时或暂无数据：
            // 先再次检查并恢复可能的错误/溢出（因为在等待期间可能发生）
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                continue;
            }
            // 检查命令解析器是否因超时而产生事件（例如半帧超时）
            dispatch_result = CameraUartDispatcher_CheckTimeout(
                &s_camera_uart_dispatcher,
                HAL_GetTick(),
                &dispatch_event);
            // 处理解析器产生的事件（若有效）
            Camera_RTOS_ProcessDispatchEvent(dispatch_result, &dispatch_event);
            // 记录一次“无数据”事件（用于统计空闲次数）
            Camera_RTOS_RecordUartNone();

            // 周期采样任务栈使用量（例如每秒一次）
            current_tick = HAL_GetTick();
            if ((current_tick - stack_sample_tick) >=
                CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS)
            {
                Camera_RTOS_UpdateCameraServiceStackStats();
                stack_sample_tick = current_tick;
            }
            continue;  // 本次循环结束，等待下一轮读取
        }

        // 处理读取到的每一个字节
        for (i = 0U; i < received; ++i)
        {
            // 在每字节处理前，检查是否有新的错误/溢出发生，若有则放弃当前块
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                break;  // 退出循环，丢弃剩余字节
            }

            // 将当前字节送入命令分发器解析
            dispatch_result = CameraUartDispatcher_FeedByte(
                &s_camera_uart_dispatcher,
                rx_chunk[i],
                HAL_GetTick(),
                &dispatch_event);
            // 处理解析器返回的事件（如完整命令、错误等）
            Camera_RTOS_ProcessDispatchEvent(dispatch_result, &dispatch_event);
        }

        // 周期采样任务栈使用量
        current_tick = HAL_GetTick();
        if ((current_tick - stack_sample_tick) >=
            CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS)
        {
            Camera_RTOS_UpdateCameraServiceStackStats();
            stack_sample_tick = current_tick;
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
        Camera_RTOS_UpdateMonitorHealthStats();
    }
}

// 获取运行统计信息的只读指针
const CameraRtosStats_t *Camera_RTOS_GetStats(void)
{
    return &s_camera_rtos_stats;
}
