#include "camera_rtos.h"             // Camera RTOS 对外错误码、健康统计和任务入口

#include "camera_cli.h"              // 文本 CLI 解析、配置读取和出队命令执行接口
#include "camera_capture.h"          // CaptureRequestQueue、同步采集结果和 CaptureTask 健康数据
#include "camera_command.h"          // 统一命令值对象、CommandQueue 和最小运行统计
#include "camera_frame_buffer.h"     // front/back 双缓冲提交和稳定 front frame 访问
#include "camera_image_process.h"    // BYPASS、GRAY、BINARY 图像处理及统计
#include "camera_pc_dump.h"          // 文本 DUMP 解析、OV56RGB5 发送和 DCMI back 地址
#include "camera_snapshot_control.h" // SDIO takeover 期间阻止 DUMP/binary request 的软件保护
#include "camera_uart_dispatcher.h"  // 在同一 UART 字节流中区分文本与二进制图像请求
#include "bsp_log.h"                 // IWDG 停止刷新时输出一次故障原因
#include "cmsis_os.h"                // CMSIS-RTOS2 延时、当前任务和栈余量查询接口
#include "stm32f4xx_hal_iwdg.h"      // IWDG 寄存器宏、配置类型和刷新操作
#include "uart_rx_dma.h"             // USART1 RX DMA、StreamBuffer 读取及错误恢复接口

#include "FreeRTOS.h"                // CommandQueue 创建失败时使用现有 configASSERT 机制
#include <stddef.h>                   // 提供 NULL 和 size_t

//============================================================================
// @file    camera_rtos.c
// @brief   摄像头业务串行执行、UART 请求分发和运行健康监控
//
// 本模块主要负责：
// 1. 在 CommTask 中读取 USART1 RX DMA 的 StreamBuffer 并维护文本/binary parser；
// 2. 将文本 CLI/DUMP 或 binary image request 转换为 CameraCommand_t 并提交队列；
// 3. 从 CommandQueue 取出命令并串行启动原有业务，通过 CaptureTask 请求 DCMI 快照；
// 4. 按 back→front→图像处理→再次提交的顺序发布稳定 RGB565 帧；
// 5. 让文本 DUMP、二进制请求和 SD SNAPSHOT 复用同一帧准备路径；
// 6. 在 MonitorTask 中采样任务栈、Heap、心跳并决定是否刷新 IWDG。
//
// 任务上下文关系：CommTask 只解析和提交；ControlTask 是 CommandQueue consumer，
// 串行执行所有控制、图像和 SD 业务；CaptureTask 是 DCMI/DMA 唯一任务级所有者。
// DCMI 帧事件 ISR 只通知 CaptureTask，不在中断中做图像处理、UART 或 FatFs 操作。
// MonitorTask 不参与摄像头业务，只观察健康状态并独占 IWDG 刷新职责。
//
// 数据关系：DCMI/算法写 back，整帧完成后 commit 为 front；DUMP 读取 front，
// SD SNAPSHOT 先复制 front 到 staging，再进行会占用共享引脚的 SDIO takeover。
//============================================================================

// 每轮最多从 StreamBuffer 取 32 字节：局部数组保持很小，同时逐字节分发的延迟可控。
#define CAMERA_RTOS_UART_RX_CHUNK_SIZE             32U

// 最多等待 UART 数据 100 ms；超时会返回主循环检查半帧超时、更新心跳和栈统计。
// 这既避免无数据时 CPU 空转，也避免永久阻塞后无法观测任务健康。
#define CAMERA_RTOS_UART_READ_TIMEOUT_MS           100U

// 每 1000 ms 采样一次 stack high-water mark，即任务运行以来“曾经最少剩余”的栈字节数。
// 数值越小表示栈峰值越接近上限；它是历史最小值，不是某一瞬间的普通空闲量。
#define CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS         1000U

// 公共 DUMP 路径的来源标签，只用于软件保护和统计分类，不写入任何 UART 协议字段。
#define CAMERA_RTOS_DUMP_SOURCE_TEXT                   0U
#define CAMERA_RTOS_DUMP_SOURCE_BINARY                 1U

// LSI 约 32 kHz 时，(999 + 1)×256/32000 ≈ 8 秒，留出正常图像/SD 操作时间。
#define CAMERA_RTOS_IWDG_PRESCALER                  IWDG_PRESCALER_256
#define CAMERA_RTOS_IWDG_RELOAD                     999U

// CommTask 最长允许 6 秒没有新心跳，低于约 8 秒硬件复位窗口以留出复位余量。
#define CAMERA_RTOS_IWDG_COMM_AGE_LIMIT_MS          6000U

// ControlTask 执行业务时沿用 6 秒上限；阻塞等待 CommandQueue 时不按年龄误判。
#define CAMERA_RTOS_IWDG_CONTROL_AGE_LIMIT_MS       6000U

// MonitorTask 正常每 1 秒运行一次，3 秒阈值可容忍短暂调度延迟但能识别真正停滞。
#define CAMERA_RTOS_IWDG_MONITOR_AGE_LIMIT_MS       3000U

// 写入 IWDG 分频/重装值后最多轮询 100 ms，防止寄存器异常时初始化永久忙等。
#define CAMERA_RTOS_IWDG_UPDATE_TIMEOUT_MS          100U

// RVU/PVU 表示重装值或分频值仍在更新；两位都清零后新配置才可安全使用。
#define CAMERA_RTOS_IWDG_UPDATE_FLAGS               (IWDG_SR_RVU | IWDG_SR_PVU)

// 调度器启动前设置；CommTask 用于 RX，ControlTask 使用同一稳定句柄发送响应。
// 该指针不是 ISR 通信变量，初始化完成后运行期不再切换 UART 实例。
static UART_HandleTypeDef *s_camera_rtos_uart;

// 跨 CommTask、ControlTask、MonitorTask、STATUS 和 FreeRTOS Hook 共享的诊断状态。
// 各字段为 volatile，但整个结构不是事务快照；它只用于健康监控而不参与业务同步。
static CameraRtosStats_t s_camera_rtos_stats;

// ControlTask 在 portMAX_DELAY 阻塞等待队列时置 1，MonitorTask 不把正常 Blocked 误判超时。
static volatile uint8_t s_camera_control_waiting_for_command;

// 保存 UART 混合协议解析状态，仅由 CommTask 初始化、喂入字节和复位。
// ISR 只把字节搬入 StreamBuffer，不直接接触此状态机。
static CameraUartDispatcher_t s_camera_uart_dispatcher;

// OV56RGB5 响应 frame_id，从 1 开始并只在整帧成功发送后递增。
// 它与 binary request 自带的 sequence 是两个不同概念，不能互相替代。
static uint32_t s_camera_rtos_frame_id = 1U;

// IWDG 句柄在调度器启动前配置；运行期只有 MonitorTask 根据全局健康条件刷新。
static IWDG_HandleTypeDef s_camera_rtos_iwdg;

// 在 Camera_RTOS_Init() 中建立干净的诊断基线，不改变 UART、DCMI 或缓冲区状态。
// 字段按协议统计、资源监控、Hook/心跳和 IWDG 四组清零，避免上次运行残值进入 STATUS。
static void Camera_RTOS_ClearStats(void)
{
    // 文本/二进制图像请求共用的 DUMP 结果统计。
    s_camera_rtos_stats.dump_request_count = 0U;
    s_camera_rtos_stats.dump_success_count = 0U;
    s_camera_rtos_stats.dump_error_count = 0U;

    // UART 空闲、文本半行及二进制协议解析分类统计。
    s_camera_rtos_stats.uart_none_count = 0U;
    s_camera_rtos_stats.uart_pending_count = 0U;
    s_camera_rtos_stats.comm_rx_count = 0U;
    s_camera_rtos_stats.comm_parse_error_count = 0U;
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

    // 最近业务结果、任务运行时间以及栈/Heap 最低余量。
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_NONE;
    s_camera_rtos_stats.last_dump_time_ms = 0U;
    s_camera_rtos_stats.uptime_ms = 0U;
    s_camera_rtos_stats.comm_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.control_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.capture_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.monitor_stack_min_free_bytes = 0U;
    s_camera_rtos_stats.free_heap_bytes = 0U;
    s_camera_rtos_stats.min_ever_free_heap_bytes = 0U;

    // 严重 Hook 和三个任务的心跳由不同上下文更新，启动时统一归零。
    s_camera_rtos_stats.hook_fault_code = 0U;
    s_camera_rtos_stats.hook_fault_count = 0U;
    s_camera_rtos_stats.assert_line = 0U;
    s_camera_rtos_stats.comm_heartbeat_count = 0U;
    s_camera_rtos_stats.control_heartbeat_count = 0U;
    s_camera_rtos_stats.monitor_heartbeat_count = 0U;
    s_camera_rtos_stats.comm_heartbeat_ms = 0U;
    s_camera_rtos_stats.control_heartbeat_ms = 0U;
    s_camera_rtos_stats.monitor_heartbeat_ms = 0U;
    s_camera_rtos_stats.comm_heartbeat_age_ms = 0U;
    s_camera_rtos_stats.control_heartbeat_age_ms = 0U;
    s_camera_rtos_stats.monitor_heartbeat_age_ms = 0U;
    s_camera_control_waiting_for_command = 0U;

    // IWDG 尚未初始化，因此先标记为禁用且没有跳过记录。
    s_camera_rtos_stats.iwdg_enabled = 0U;
    s_camera_rtos_stats.iwdg_refresh_skip_count = 0U;
    s_camera_rtos_stats.iwdg_last_skip_reason = CAMERA_RTOS_IWDG_SKIP_NONE;
}

// 为当前未链接 HAL IWDG 源文件的构建提供最小兼容初始化。
// 调用位置仍遵守 HAL 接口：先验证句柄和配置，再启动 IWDG、开放写保护、写入
// 分频/重装寄存器，并等待硬件完成更新。IWDG 一旦启动便只能由复位停止。
HAL_StatusTypeDef HAL_IWDG_Init(IWDG_HandleTypeDef *hiwdg)
{
    uint32_t tick_start;  // 记录寄存器更新轮询起点，用于限定最长等待时间

    // 空句柄、错误实例或超出 HAL 允许范围的参数都不能写入 IWDG 寄存器。
    if ((hiwdg == NULL) || (hiwdg->Instance != IWDG) ||
        (IS_IWDG_PRESCALER(hiwdg->Init.Prescaler) == 0U) ||
        (IS_IWDG_RELOAD(hiwdg->Init.Reload) == 0U))
    {
        return HAL_ERROR;
    }

    __HAL_IWDG_START(hiwdg);
    IWDG_ENABLE_WRITE_ACCESS(hiwdg);
    hiwdg->Instance->PR = hiwdg->Init.Prescaler;
    hiwdg->Instance->RLR = hiwdg->Init.Reload;

    // RVU/PVU 由硬件在配置同步完成后清零；循环只有“更新完成”或“100 ms 超时”
    // 两个出口。这里尚未启动调度器，不能使用 osDelay，因此采用带超时的短轮询。
    tick_start = HAL_GetTick();
    while ((hiwdg->Instance->SR & CAMERA_RTOS_IWDG_UPDATE_FLAGS) != 0U)
    {
        if ((HAL_GetTick() - tick_start) > CAMERA_RTOS_IWDG_UPDATE_TIMEOUT_MS)
        {
            return HAL_TIMEOUT;
        }
    }

    __HAL_IWDG_RELOAD_COUNTER(hiwdg);
    return HAL_OK;
}

// 为当前构建提供最小 HAL 刷新接口：验证目标确为 IWDG 后写入重装键。
// 本函数不判断系统是否健康；是否允许刷新由 MonitorTask 的 ServiceIwdg() 决定。
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg)
{
    if ((hiwdg == NULL) || (hiwdg->Instance != IWDG))
    {
        return HAL_ERROR;
    }

    __HAL_IWDG_RELOAD_COUNTER(hiwdg);
    return HAL_OK;
}

// 查询“调用本函数的当前任务”自启动以来历史最小剩余栈空间，单位为字节。
// CMSIS-RTOS2 的该值等价于 stack high-water mark，用于发现最坏时刻的栈压力，
// 因而必须分别在 CommTask、ControlTask 和 MonitorTask 自己的上下文中调用。
static uint32_t Camera_RTOS_GetCurrentTaskStackMinFreeBytes(void)
{
    return osThreadGetStackSpace(osThreadGetId());
}

// 仅在 CommTask 上下文采样自身 stack high-water mark。
static void Camera_RTOS_UpdateCommStackStats(void)
{
    s_camera_rtos_stats.comm_stack_min_free_bytes =
        Camera_RTOS_GetCurrentTaskStackMinFreeBytes();
}

// 仅在 ControlTask 上下文采样自身栈；业务调用链仍含 DUMP、SD 和 FatFs 控制。
static void Camera_RTOS_UpdateControlStackStats(void)
{
    s_camera_rtos_stats.control_stack_min_free_bytes =
        Camera_RTOS_GetCurrentTaskStackMinFreeBytes();
}

// 在 MonitorTask 上下文同时采样自身历史最小栈余量、当前 Heap 和历史最小 Heap。
// 当前 Heap 反映即时余量，minimum-ever 值则保留系统运行以来最紧张的内存时刻。
static void Camera_RTOS_UpdateMonitorHealthStats(void)
{
    const CameraCaptureStats_t *capture_stats = Camera_CaptureGetStats();

    s_camera_rtos_stats.monitor_stack_min_free_bytes =
        Camera_RTOS_GetCurrentTaskStackMinFreeBytes();
    s_camera_rtos_stats.capture_stack_min_free_bytes =
        (capture_stats != NULL) ? capture_stats->stack_min_free_bytes : 0U;
    s_camera_rtos_stats.free_heap_bytes = (uint32_t)xPortGetFreeHeapSize();
    s_camera_rtos_stats.min_ever_free_heap_bytes =
        (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

// 使用同一个 current_tick 计算三个任务距离最近一次心跳已经过去多久。
// tick 小于已保存时间时按 0 处理，避免 tick 回退或回绕边界产生无符号巨值并误判超时。
static void Camera_RTOS_UpdateHeartbeatAges(uint32_t current_tick)
{
    s_camera_rtos_stats.comm_heartbeat_age_ms =
        (current_tick >= s_camera_rtos_stats.comm_heartbeat_ms) ?
        (current_tick - s_camera_rtos_stats.comm_heartbeat_ms) : 0U;
    s_camera_rtos_stats.control_heartbeat_age_ms =
        (current_tick >= s_camera_rtos_stats.control_heartbeat_ms) ?
        (current_tick - s_camera_rtos_stats.control_heartbeat_ms) : 0U;
    s_camera_rtos_stats.monitor_heartbeat_age_ms =
        (current_tick >= s_camera_rtos_stats.monitor_heartbeat_ms) ?
        (current_tick - s_camera_rtos_stats.monitor_heartbeat_ms) : 0U;
}

// 将 Hook 与三个任务的启动/超时状态按优先顺序归纳为一个 IWDG 跳过原因。
// 该函数只做判定，不直接刷新硬件；统一原因使 STATUS 和故障日志能解释为何不喂狗。
static CameraRtosIwdgSkipReason_t Camera_RTOS_GetIwdgSkipReason(
    uint32_t current_tick)
{
    Camera_RTOS_UpdateHeartbeatAges(current_tick);

    // 严重 Hook 说明内核已进入不可信状态，优先级高于普通任务心跳问题。
    if (s_camera_rtos_stats.hook_fault_code != 0U)
    {
        return CAMERA_RTOS_IWDG_SKIP_HOOK_FAULT;
    }
    // count 为 0 表示任务从未进入主循环，不能把初始时间戳误当成健康心跳。
    if (s_camera_rtos_stats.comm_heartbeat_count == 0U)
    {
        return CAMERA_RTOS_IWDG_SKIP_COMM_NOT_STARTED;
    }
    if (s_camera_rtos_stats.control_heartbeat_count == 0U)
    {
        return CAMERA_RTOS_IWDG_SKIP_CONTROL_NOT_STARTED;
    }
    if (s_camera_rtos_stats.monitor_heartbeat_count == 0U)
    {
        return CAMERA_RTOS_IWDG_SKIP_MONITOR_NOT_STARTED;
    }
    if (s_camera_rtos_stats.comm_heartbeat_age_ms >
        CAMERA_RTOS_IWDG_COMM_AGE_LIMIT_MS)
    {
        return CAMERA_RTOS_IWDG_SKIP_COMM_TIMEOUT;
    }
    if ((s_camera_control_waiting_for_command == 0U) &&
        (s_camera_rtos_stats.control_heartbeat_age_ms >
         CAMERA_RTOS_IWDG_CONTROL_AGE_LIMIT_MS))
    {
        return CAMERA_RTOS_IWDG_SKIP_CONTROL_TIMEOUT;
    }
    if (s_camera_rtos_stats.monitor_heartbeat_age_ms >
        CAMERA_RTOS_IWDG_MONITOR_AGE_LIMIT_MS)
    {
        return CAMERA_RTOS_IWDG_SKIP_MONITOR_TIMEOUT;
    }

    return CAMERA_RTOS_IWDG_SKIP_NONE;
}

// 仅由 MonitorTask 调用：所有健康条件满足才刷新 IWDG，否则持续停止喂狗等待复位。
// 把刷新职责放在 MonitorTask，可防止 Comm/Control 卡死后仍由业务路径自我喂狗。
static void Camera_RTOS_ServiceIwdg(uint32_t current_tick)
{
    CameraRtosIwdgSkipReason_t skip_reason;  // 本周期综合健康判定结果
    uint32_t previous_skip_reason;           // 上周期原因，用于抑制相同错误重复刷屏

    // 初始化失败或尚未启用时没有可刷新的硬件，监控统计仍照常运行。
    if (s_camera_rtos_stats.iwdg_enabled == 0U)
    {
        return;
    }

    skip_reason = Camera_RTOS_GetIwdgSkipReason(current_tick);
    // 只有“三个任务已启动、活动任务未超时且无 Hook 故障”才写 IWDG 重装键。
    if (skip_reason == CAMERA_RTOS_IWDG_SKIP_NONE)
    {
        (void)HAL_IWDG_Refresh(&s_camera_rtos_iwdg);
        return;
    }

    previous_skip_reason = s_camera_rtos_stats.iwdg_last_skip_reason;
    s_camera_rtos_stats.iwdg_refresh_skip_count++;
    s_camera_rtos_stats.iwdg_last_skip_reason = (uint32_t)skip_reason;

    // 同一种正式异常只输出一次；后续周期继续累计跳过次数并保持不喂狗，
    // 避免反复日志占用 UART，同时给硬件看门狗留下完成复位的机会。
    if (previous_skip_reason != (uint32_t)skip_reason)
    {
        LOG_ERROR("IWDG refresh skipped, reason=%u", (unsigned int)skip_reason);
    }
}

// 记录一次进入公共图像发送路径的请求；文本 DUMP 和合法二进制请求都会调用。
// 该计数在真正采集前递增，因此也包含随后被 SNAPSHOT guard 阻止的请求。
void Camera_RTOS_RecordDumpRequest(void)
{
    s_camera_rtos_stats.dump_request_count++;
}

// 记录一次“帧准备 + OV56RGB5 发送”完整成功，并保存此次端到端耗时。
// 只在 ControlTask 的公共 DUMP 路径调用，不参与 frame_id 的递增规则。
void Camera_RTOS_RecordDumpSuccess(uint32_t elapsed_ms)
{
    s_camera_rtos_stats.dump_success_count++;
    s_camera_rtos_stats.last_dump_time_ms = elapsed_ms;
}

// 记录一次公共 DUMP 失败；若调用方没有给出具体码，则保存通用 DUMP_FAILED。
// 这样 STATUS 中的失败计数与 last_error_code 不会出现“失败但错误码为 NONE”的矛盾。
void Camera_RTOS_RecordDumpError(uint32_t error_code)
{
    s_camera_rtos_stats.dump_error_count++;
    s_camera_rtos_stats.last_error_code =
        (error_code == CAMERA_RTOS_ERR_NONE) ? CAMERA_RTOS_ERR_DUMP_FAILED : error_code;
}

// 记录 CommTask 一次限时读取没有收到字节，用于区分正常 UART 空闲与错误恢复。
void Camera_RTOS_RecordUartNone(void)
{
    s_camera_rtos_stats.uart_none_count++;
}

// 记录文本解析器仍处于“命令尚未以 CR/LF 结束”的中间状态，便于观察半行输入。
void Camera_RTOS_RecordUartPending(void)
{
    s_camera_rtos_stats.uart_pending_count++;
}

// 从 FreeRTOS 严重错误 Hook 记录最小故障信息，供 MonitorTask 判定停止刷新 IWDG。
// Hook 触发时调度器或 Heap 可能已经不可靠，因此这里只写静态 volatile 字段，
// 不输出 UART、不等待锁、不分配内存，也不尝试在故障上下文直接恢复业务。
void Camera_RTOS_RecordHookFault(uint32_t fault_code, uint32_t assert_line)
{
    s_camera_rtos_stats.hook_fault_code = fault_code;
    s_camera_rtos_stats.hook_fault_count++;
    s_camera_rtos_stats.assert_line = assert_line;
}

// 在 CommTask 上下文检查并恢复 UART DMA 错误，ISR 不执行这些复杂操作。
// 返回 0 表示输入链路可继续；返回 1 表示本轮字节已不再可信，主循环必须重新开始。
// 真正重启成功时还会复位两个解析器并排空旧字节，避免错误前后的半帧被拼接。
static uint8_t Camera_RTOS_RecoverUartInputIfNeeded(void)
{
    UartRxDmaRecoveryResult_t recovery_result;  // 无错误、稍后重试或已完成恢复

    recovery_result = UART_RxDma_RecoverIfNeeded();

    // 没有待恢复错误时保留当前协议状态，调用者可以继续处理已有字节。
    if (recovery_result == UART_RX_DMA_RECOVERY_NONE)
    {
        return 0U;
    }

    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_DMA_RECOVERY;
    // HAL/UART 暂时仍忙时延时 1 ms 让出 CPU，下轮主循环再尝试；不在此忙等。
    if (recovery_result == UART_RX_DMA_RECOVERY_RETRY)
    {
        (void)osDelay(1U);
        return 1U;
    }

    // DMA 已恢复后，恢复前残留的文本行或二进制半帧都必须整体作废。
    CameraUartDispatcher_Reset(&s_camera_uart_dispatcher);
    Camera_PC_Dump_ResetCommandParser();
    (void)UART_RxDma_Drain();
    if (UART_RxDma_HasOverflow() != 0U)
    {
        UART_RxDma_ClearOverflow();
    }
    return 1U;
}

// 处理 ISR 向 StreamBuffer 写入过快造成的溢出；一旦丢字节，当前协议帧便无法可信解析。
// 因此复位文本/二进制状态机、排空现有残片后再清 overflow 标志，而不是尝试续接。
// 返回 1 提醒 CommTask 放弃当前 chunk，0 表示没有发生溢出。
static uint8_t Camera_RTOS_DiscardOverflowedInput(void)
{
    // 无溢出时不能清解析器，否则每次检查都会破坏正常的跨 chunk 命令/请求。
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

// 在 ControlTask 中同步准备一帧最终可读的 RGB565 front frame。
// CommTask 只把文本 DUMP、binary request 或 SD SNAPSHOT 转成 Command；ControlTask
// 出队后调用本函数。CaptureTask 负责摄像头 HAL 和超时停止；commit 和图像处理仍在 ControlTask。
//
// 处理顺序是：CaptureTask 启动 DCMI 写 back → ISR 通知完成 → 原始帧 commit 为 front →
// 图像算法读该 front、写另一个 back → 再次 commit，最终 front 才交给 DUMP/SD。
// 两条业务链复用这里，是为了避免各自维护不同的采集时序、超时和处理模式规则。
uint32_t Camera_RTOS_PrepareRgb565Frame(uint32_t timeout_ms)
{
    CameraCaptureResult_t capture_ret;          // CaptureTask 返回的启动、完成、超时或 HAL 错误
    const CameraCaptureStats_t *capture_stats;  // 保留 DCMI 启动 helper 的具体子错误
    CameraFrameBufferStatus_t commit_ret;      // 将已完成 back 发布为 front 的结果
    CameraImageProcessStatus_t process_ret;    // BYPASS/GRAY/BINARY 处理及第二次提交结果
    CameraProcessMode_t process_mode;          // CLI 当前选择的处理模式，本次准备期间保持该快照值
    uint8_t binary_threshold;                  // BINARY 模式使用的 0~255 灰度阈值快照

    // 零超时无法给 ISR 留出任何完成机会，也会让调用语义不明确，因此按状态错误拒绝。
    if (timeout_ms == 0U)
    {
        return CAMERA_RTOS_ERR_BAD_STATE;
    }

    // CaptureTask owns DCMI/DMA and the current back buffer until raw capture ends.
    // This synchronous call blocks ControlTask on a completion notification, not polling.
    capture_ret = Camera_CaptureRequestFrame(timeout_ms);
    if (capture_ret == CAMERA_CAPTURE_START_FAILED)
    {
        capture_stats = Camera_CaptureGetStats();
        return CAMERA_RTOS_ERR_SNAPSHOT_START_BASE |
            ((capture_stats != NULL) ? capture_stats->last_start_status : 0U);
    }
    if (capture_ret == CAMERA_CAPTURE_TIMEOUT)
    {
        return CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT;
    }
    if (capture_ret == CAMERA_CAPTURE_HAL_ERROR)
    {
        return CAMERA_RTOS_ERR_CAPTURE_HAL;
    }
    if (capture_ret != CAMERA_CAPTURE_OK)
    {
        return CAMERA_RTOS_ERR_BAD_STATE;
    }

    // 第一次 commit 发布 DCMI 原始 RGB565：本次 back 变为稳定 front，旧 front
    // 变成可供图像处理写入的新 back。DUMP/SD 仍不会直接读取正在写的 back。
    commit_ret = Camera_FrameBuffer_CommitBackBuffer();
    if (commit_ret != CAMERA_FB_OK)
    {
        // 高位标识双缓冲发布阶段，低位保留 CameraFrameBufferStatus_t 子错误。
        return CAMERA_RTOS_ERR_CAPTURE_COMMIT_BASE | (uint32_t)commit_ret;
    }

    // 在本次处理开始前读取一次 CLI 配置；BYPASS 也会把原始 front 复制到 back 并
    // 计算统计，使三种模式都遵守“读稳定 front、写 back、成功后 commit”的规则。
    process_mode = Camera_CLI_GetProcessMode();
    binary_threshold = Camera_CLI_GetBinaryThreshold();

    // ApplyToFrameBuffer() 从原始 front 读取，向当前 back 写处理结果；只有算法完整
    // 成功后它才执行第二次 commit。返回成功时，最终 front 就是 DUMP 与 SD 共用帧。
    process_ret = Camera_ImageProcess_ApplyToFrameBuffer(process_mode, binary_threshold);
    if (process_ret != CAMERA_PROCESS_OK)
    {
        // 用户选择的 GRAY/BINARY 等处理失败时，回退到 BYPASS，把稳定原始 front
        // 完整复制到 back 并提交，尽量保留可发送/保存的一帧而不是发布半成品。
        process_ret = Camera_ImageProcess_ApplyToFrameBuffer(
            CAMERA_PROCESS_MODE_BYPASS,
            binary_threshold);
        if (process_ret != CAMERA_PROCESS_OK)
        {
            // 连 BYPASS 都失败说明帧视图、尺寸或提交链路已异常，不能宣称准备成功。
            return CAMERA_RTOS_ERR_IMAGE_PROCESS_BASE | (uint32_t)process_ret;
        }
        // BYPASS 回退成功后最终 front 仍是一帧完整原始 RGB565，允许业务继续。
    }

    return CAMERA_RTOS_ERR_NONE;
}

// 把公共帧准备与 OV56RGB5 发送串成一次完整 DUMP，供文本和二进制请求共同调用。
// 准备成功后 Camera_PC_Dump_SendFrame() 只读取最终 front；它不会读取仍可被下一轮
// DCMI/算法写入的 back。frame_id 仅在完整 UART 帧成功发送后递增，失败时保留原值。
static uint32_t Camera_RTOS_PrepareAndSend(void)
{
    uint8_t dump_ret;          // OV56RGB5 header、payload 或 CRC 发送阶段的子错误
    uint32_t prepare_result;   // DCMI、commit、图像处理公共准备结果

    // 文本 DUMP 与 binary request 使用相同的 3000 ms 采集上限和处理模式。
    prepare_result = Camera_RTOS_PrepareRgb565Frame(
        CAMERA_RTOS_RGB565_PREPARE_TIMEOUT_MS);
    if (prepare_result != CAMERA_RTOS_ERR_NONE)
    {
        return prepare_result;
    }

    // 发送模块从稳定 front 构造 OV56RGB5 header，发送 payload 并附加 CRC32。
    dump_ret = Camera_PC_Dump_SendFrame(
        s_camera_rtos_uart,
        s_camera_rtos_frame_id);
    if (dump_ret != 0U)
    {
        // 高位基础码定位 UART 帧发送阶段，低位保留发送 helper 的具体子错误。
        return CAMERA_RTOS_ERR_DUMP_SEND_BASE | (uint32_t)dump_ret;
    }

    // 只有 header、完整 payload 和 CRC 都发送成功，下一次响应才使用新 frame_id。
    s_camera_rtos_frame_id++;
    return CAMERA_RTOS_ERR_NONE;   // 公共准备和 UART 发送均已完成
}

// 在 ControlTask 内统一执行文本 DUMP 与 binary image request，并先检查快照保护。
// SD SNAPSHOT takeover 会暂时改变摄像头/SDIO 共享硬件；guard 禁止此时再启动 DCMI，
// 避免两条路径同时驱动 PC8/PC9/PC11。request_source 只决定阻止统计和是否输出文本，
// 成功路径始终复用相同的 PrepareAndSend()，不会形成两套采集/协议实现。
static uint8_t Camera_RTOS_ProcessDumpRequest(uint8_t request_source)
{
#if (CAMERA_SD_DIAG_SD_ONLY_BOOT != 0U)
    static const uint8_t sd_only_text[] =
        "DUMP blocked: SD_ONLY_BOOT_NO_CAMERA.\r\n";
#endif
    static const uint8_t blocked_text[] =
        "DUMP blocked: snapshot software guard active.\r\n";
    uint32_t error_code;       // 公共准备或 OV56RGB5 发送返回的分阶段错误码
    uint32_t dump_start_tick;  // 端到端 DUMP 计时起点
    uint32_t dump_elapsed_ms;  // 包含采集、处理和阻塞式 UART 发送的总耗时

    // 请求一进入公共路径即计数，因此被 SD-only/guard 阻止的请求也可在 STATUS 中观察。
    Camera_RTOS_RecordDumpRequest();

#if (CAMERA_SD_DIAG_SD_ONLY_BOOT != 0U)
    // 历史 SD-only 诊断构建没有可用摄像头链路，任何图像请求都必须被明确阻止。
    if (request_source == CAMERA_RTOS_DUMP_SOURCE_BINARY)
    {
        Camera_SnapshotControl_RecordBinaryBlocked();
    }
    else
    {
        Camera_SnapshotControl_RecordDumpBlocked();
    }

    // 此诊断信息属于文本控制面；构建开关启用时按原行为通过 UART 输出。
    if (s_camera_rtos_uart != NULL)
    {
        (void)HAL_UART_Transmit(
            s_camera_rtos_uart,
            (uint8_t *)sd_only_text,
            (uint16_t)(sizeof(sd_only_text) - 1U),
            HAL_MAX_DELAY);
    }

    s_camera_rtos_stats.last_dump_time_ms = 0U;
    Camera_RTOS_RecordDumpError(CAMERA_RTOS_ERR_SD_ONLY_BOOT_NO_CAMERA);
    return 0U;
#endif

    // guard 为 0 表示 SD SNAPSHOT 已占用或正在切换共享硬件，不能启动新的 DCMI 快照。
    if (Camera_SnapshotControl_IsDumpAllowed() == 0U)
    {
        if (request_source == CAMERA_RTOS_DUMP_SOURCE_BINARY)
        {
            // 二进制请求被阻止时只更新统计，不插入 ASCII 文本，避免破坏 PC 协议同步。
            Camera_SnapshotControl_RecordBinaryBlocked();
        }
        else
        {
            Camera_SnapshotControl_RecordDumpBlocked();
            if (s_camera_rtos_uart != NULL)
            {
                // 文本 DUMP 允许返回可读原因；阻塞发送保持既有控制台输出顺序。
                (void)HAL_UART_Transmit(
                    s_camera_rtos_uart,
                    (uint8_t *)blocked_text,
                    (uint16_t)(sizeof(blocked_text) - 1U),
                    HAL_MAX_DELAY);
            }
        }

        s_camera_rtos_stats.last_dump_time_ms = 0U;
        Camera_RTOS_RecordDumpError(CAMERA_RTOS_ERR_SNAPSHOT_GUARD_ACTIVE);
        return 0U;
    }

    // 计时覆盖 DCMI 等待、图像处理及 UART 整帧发送，用于 STATUS 的 last_dump_time_ms。
    dump_start_tick = HAL_GetTick();
    error_code = Camera_RTOS_PrepareAndSend();
    dump_elapsed_ms = HAL_GetTick() - dump_start_tick;
    Camera_RTOS_UpdateControlStackStats();
    if (error_code == CAMERA_RTOS_ERR_NONE)
    {
        Camera_RTOS_RecordDumpSuccess(dump_elapsed_ms);
        return 1U;
    }

    s_camera_rtos_stats.last_dump_time_ms = dump_elapsed_ms;
    Camera_RTOS_RecordDumpError(error_code);
    return 0U;
}

// 记录已通过 dispatcher 完整校验并从 CommandQueue 取出的 binary image request。
// seq 是 PC 请求序号，仅用于关联和诊断，不改变 OV56RGB5 响应 frame_id。
static void Camera_RTOS_RecordBinaryRequest(uint16_t seq)
{
    s_camera_rtos_stats.binary_request_count++;
    s_camera_rtos_stats.last_binary_request_seq = seq;
}

// 记录一次 binary request 解析失败，并把总数拆分到 CRC、版本、类型、长度或 EOF。
// 该函数只消费 dispatcher 已给出的结果，不在这里重新解析字节，也不发送文本响应。
static void Camera_RTOS_RecordBinaryError(
    ImageRequestParseResult_t parse_result)
{
    s_camera_rtos_stats.comm_parse_error_count++;
    s_camera_rtos_stats.binary_request_error_count++;
    s_camera_rtos_stats.last_binary_error_code = (uint32_t)parse_result;

    // switch 只为已定义的协议错误增加分类计数；其他结果仍保留在总错误和 last code 中。
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

// 记录 binary request 在规定时间内未收齐的半帧超时。
// 不输出 ASCII 错误文本，防止 PC 正在等待二进制响应时被额外字节打乱帧边界。
static void Camera_RTOS_RecordBinaryTimeout(void)
{
    s_camera_rtos_stats.comm_parse_error_count++;
    s_camera_rtos_stats.binary_request_timeout_count++;
    s_camera_rtos_stats.last_binary_error_code = IMAGE_REQUEST_PARSE_TIMEOUT;
}

// 向统一队列提交一个完整值对象；失败只记录软件状态，不阻塞 parser 或触发复位。
static uint8_t Camera_RTOS_SubmitCommand(const CameraCommand_t *command)
{
    if (Camera_CommandSubmit(command) == false)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_COMMAND_QUEUE;
        return 0U;
    }

    return 1U;
}

// ControlTask 是唯一命令执行者；各分支继续调用已验证的 CLI、DUMP 和 SD 路径。
static void Camera_RTOS_ExecuteCommand(const CameraCommand_t *command)
{
    if (command == NULL)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
        return;
    }

    switch (command->type)
    {
        case CAMERA_CMD_HELP:
        case CAMERA_CMD_STATUS:
        case CAMERA_CMD_CLI_ERROR:
        case CAMERA_CMD_PROC_GET:
        case CAMERA_CMD_PROC_SET:
        case CAMERA_CMD_THRESHOLD_GET:
        case CAMERA_CMD_THRESHOLD_SET:
        case CAMERA_CMD_RESET:
        case CAMERA_CMD_SD_STATUS:
        case CAMERA_CMD_SD_SNAPSHOT:
            (void)Camera_CLI_ExecuteCommand(s_camera_rtos_uart, command);
            break;

        case CAMERA_CMD_DUMP:
            (void)Camera_RTOS_ProcessDumpRequest(
                CAMERA_RTOS_DUMP_SOURCE_TEXT);
            break;

        case CAMERA_CMD_IMAGE_REQUEST:
            Camera_RTOS_RecordBinaryRequest(
                command->args.image_request.seq);
            if (Camera_RTOS_ProcessDumpRequest(
                    CAMERA_RTOS_DUMP_SOURCE_BINARY) != 0U)
            {
                s_camera_rtos_stats.binary_request_success_count++;
            }
            break;

        case CAMERA_CMD_NONE:
        default:
            s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
            break;
    }
}

// 将 dispatcher 判定为文本的单字节交给现有行解析器，并把 DUMP 事件提交统一队列。
// 普通 CLI 由 HandleLine() 生成命令；DUMP 仍由 FeedCommandByte() 单独识别后生成命令。
static void Camera_RTOS_ProcessTextByte(uint8_t byte)
{
    uint8_t command;  // 文本行仍未结束、已由 CLI 处理、DUMP 或异常状态
    CameraCommand_t queue_command = {0}; // DUMP 事件转换得到的无参数命令值对象

    command = Camera_PC_Dump_FeedCommandByte(s_camera_rtos_uart, byte);
    // PENDING 表示当前字节尚未形成完整行，保留解析器状态等待后续 CR/LF。
    if (command == CAMERA_PC_DUMP_CMD_PENDING)
    {
        Camera_RTOS_RecordUartPending();
        return;
    }

    // HELP/STATUS/PROC/THR/RESET/SD 等 CLI 已由文本 parser 提交 CommandQueue。
    if (command == CAMERA_PC_DUMP_CMD_CLI)
    {
        return;
    }

    // DUMP 事件转换为值对象；真正采集、处理和 OV56RGB5 发送在出队分支执行。
    if (command == CAMERA_PC_DUMP_CMD_DUMP)
    {
        queue_command.type = CAMERA_CMD_DUMP;
        (void)Camera_RTOS_SubmitCommand(&queue_command);
        return;
    }

    s_camera_rtos_stats.comm_parse_error_count++;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
}

// 将混合协议 dispatcher 的单个结果转换为文本处理、图像请求或错误统计动作。
// event 由同一任务刚调用的 dispatcher 填充，生命周期只覆盖当前处理步骤；本函数
// 不保存其地址。NONE/default 不做业务，BAD_ARGUMENT 记为内部状态错误。
static void Camera_RTOS_ProcessDispatchEvent(
    CameraUartDispatchResult_t result,
    const CameraUartDispatchEvent_t *event)
{
    CameraCommand_t command = {0}; // binary 请求只复制执行所需的 seq 进入队列

    // 每个输入字节最多产生一个事件；二进制请求只有完整通过协议校验后才进入 DUMP。
    switch (result)
    {
        case CAMERA_UART_DISPATCH_TEXT_BYTE:
            Camera_RTOS_ProcessTextByte(event->text_byte);
            break;

        case CAMERA_UART_DISPATCH_IMAGE_REQUEST:
            command.type = CAMERA_CMD_IMAGE_REQUEST;
            command.args.image_request.seq = event->image_request.seq;
            (void)Camera_RTOS_SubmitCommand(&command);
            break;

        case CAMERA_UART_DISPATCH_BINARY_ERROR:
            Camera_RTOS_RecordBinaryError(event->binary_result);
            break;

        case CAMERA_UART_DISPATCH_BINARY_TIMEOUT:
            Camera_RTOS_RecordBinaryTimeout();
            break;

        case CAMERA_UART_DISPATCH_BAD_ARGUMENT:
            s_camera_rtos_stats.comm_parse_error_count++;
            s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_BAD_STATE;
            break;

        case CAMERA_UART_DISPATCH_NONE:
        default:
            break;
    }
}

// 在调度器启动前重置模块状态并登记 UART；不创建任务，也不启动 DMA/DCMI。
// 先暂存 NULL 可保证无效参数路径不会遗留旧句柄，frame_id 每次系统初始化从 1 开始。
void Camera_RTOS_Init(UART_HandleTypeDef *huart)
{
    s_camera_rtos_uart = NULL;
    s_camera_rtos_frame_id = 1U;
    Camera_RTOS_ClearStats();

    // 无效句柄只记录错误；CommTask 启动后会每秒阻塞重试等待有效配置。
    if (huart == NULL)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_NULL;
        return;
    }

    s_camera_rtos_uart = huart;
}

// 在调度器启动前按约 8 秒窗口配置并启动 IWDG，运行期刷新权只交给 MonitorTask。
// 只有 HAL 初始化成功才设置 enabled；失败时 MonitorTask 仍运行健康统计但不会假喂狗。
HAL_StatusTypeDef Camera_RTOS_IwdgInit(void)
{
    HAL_StatusTypeDef status;  // 保存最小 HAL 初始化的成功、参数错误或更新超时结果

    s_camera_rtos_iwdg.Instance = IWDG;
    s_camera_rtos_iwdg.Init.Prescaler = CAMERA_RTOS_IWDG_PRESCALER;
    s_camera_rtos_iwdg.Init.Reload = CAMERA_RTOS_IWDG_RELOAD;

    status = HAL_IWDG_Init(&s_camera_rtos_iwdg);
    if (status == HAL_OK)
    {
        s_camera_rtos_stats.iwdg_enabled = 1U;
    }

    return status;
}

// CommTask 主循环：独占 UART RX、文本/binary parser 和 CommandQueue producer。
// UART ISR/DMA 只把字节送入 StreamBuffer；本任务不执行控制、图像或存储业务。
void Camera_RTOS_CommTask(void *argument)
{
    uint8_t rx_chunk[CAMERA_RTOS_UART_RX_CHUNK_SIZE];    // 栈上 32 字节接收块，不存放大图像
    size_t received;                                     // 本轮从 StreamBuffer 实际取出的字节数，最大 32
    size_t i;                                            // 在当前有限 rx_chunk 内逐字节喂解析器的索引
    uint32_t stack_sample_tick;                          // 上次采样 high-water mark 的 HAL tick
    uint32_t current_tick;                               // 同一轮周期判断使用的当前 HAL tick 快照
    CameraUartDispatchEvent_t dispatch_event;            // dispatcher 输出的文本字节、请求或错误详情
    CameraUartDispatchResult_t dispatch_result;          // 说明本次输入是否形成一个可处理事件

    (void)argument;  // CubeMX 任务入口保留统一参数，本任务当前不需要启动参数

    // 两级解析状态都由本任务拥有：dispatcher 先区分文本/二进制，PC Dump parser
    // 再把文本累计为 CR/LF 结尾的命令行。任务启动时先清除任何旧半帧状态。
    CameraUartDispatcher_Init(&s_camera_uart_dispatcher);
    Camera_PC_Dump_ResetCommandParser();

    // UART 句柄必须由调度器启动前的 Camera_RTOS_Init() 提供。循环在句柄有效时退出；
    // 配置缺失时没有总 timeout，因为任务无法继续其唯一业务，但每轮 osDelay(1000)
    // 都会阻塞让出 CPU，因此不是忙等待，MonitorTask 仍可运行并最终由健康机制处理。
    while (s_camera_rtos_uart == NULL)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_NULL;
        (void)osDelay(1000U);
    }

    // 启动 USART1 RX DMA 和静态 StreamBuffer。循环在 HAL_OK 时退出；暂时失败时每秒
    // 重试一次且主动阻塞，不会高速反复操作硬件。这里保持原有无总 timeout 策略，
    // 因为没有接收链路时 CommTask 无法进入正常协议主循环。
    while (UART_RxDma_Init(s_camera_rtos_uart) != HAL_OK)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_UART_DMA_INIT;
        (void)osDelay(1000U);
    }

    // 初始化完成后先建立一次栈余量基线；以后每约 1 秒更新历史最小剩余字节数。
    Camera_RTOS_UpdateCommStackStats();
    stack_sample_tick = HAL_GetTick();

    // 任务生命周期主循环没有“退出”条件，这是 FreeRTOS 服务任务的正常模型。
    // 每轮要么在 UART_RxDma_Read() 最多阻塞 100 ms，要么处理至多 32 字节；
    // 错误重试也包含 osDelay，因此该无限循环不会在空闲状态持续占满 CPU。
    for (;;)
    {
        // 每次进入 UART 服务循环发布 CommTask 心跳；ControlTask 业务阻塞不会影响它。
        s_camera_rtos_stats.comm_heartbeat_count++;
        s_camera_rtos_stats.comm_heartbeat_ms = HAL_GetTick();

        // 先处理 UART 硬件错误；返回非零表示解析连续性已失效，本轮不能继续用旧 chunk。
        if (Camera_RTOS_RecoverUartInputIfNeeded() != 0U)
        {
            continue;  // 重新进入主循环，由恢复后的 DMA/解析器接收新数据
        }

        // StreamBuffer 丢字节后 magic、length、CRC 或文本行边界都可能错位，必须整体丢弃。
        if (Camera_RTOS_DiscardOverflowedInput() != 0U)
        {
            continue;
        }

        // 从 ISR/DMA 后端的 StreamBuffer 进行“有上限的阻塞读取”：最多取满 32 字节，
        // 最长等待 100 ms。小块降低任务栈占用，也让协议超时和健康采样定期得到执行。
        received = UART_RxDma_Read(rx_chunk,
                                   sizeof(rx_chunk),
                                   CAMERA_RTOS_UART_READ_TIMEOUT_MS);
        s_camera_rtos_stats.comm_rx_count += (uint32_t)received;
        if (received == 0U)
        {
            // 等待期间 ISR 仍可能报告 DMA 错误或 StreamBuffer 溢出，因此在把 received=0
            // 当作普通空闲前再检查一次；有错误便清状态并立即开始下一轮。
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                continue;
            }
            // 无新字节时让 dispatcher 检查 binary request 半帧是否已超时；若超时，
            // 它会产生一次事件以复位协议状态，避免残缺请求永久占住解析器。
            dispatch_result = CameraUartDispatcher_CheckTimeout(
                &s_camera_uart_dispatcher,
                HAL_GetTick(),
                &dispatch_event);
            // NONE 不做业务，TIMEOUT 则只更新二进制超时统计而不插入文本响应。
            Camera_RTOS_ProcessDispatchEvent(dispatch_result, &dispatch_event);

            // 该计数描述正常空闲/超时唤醒频率，并不等价于 UART 错误次数。
            Camera_RTOS_RecordUartNone();

            // 即使 UART 一直空闲也每秒采样 high-water mark，便于 STATUS 观察最坏栈余量。
            current_tick = HAL_GetTick();
            if ((current_tick - stack_sample_tick) >=
                CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS)
            {
                Camera_RTOS_UpdateCommStackStats();
                stack_sample_tick = current_tick;
            }
            continue;  // 本轮没有业务字节，回到带超时读取而不是空转
        }

        // rx_chunk 长度由 UART_RxDma_Read() 和 32 字节数组共同限制；循环按顺序逐字节
        // 喂入有状态 dispatcher，received 个字节处理完即退出，不存在无界扫描。
        for (i = 0U; i < received; ++i)
        {
            // 解析 chunk 期间 DMA 仍在后台接收；若此时发生错误/溢出，当前块剩余字节
            // 与解析器状态可能已不连续，因此立即 break 丢弃，而不是继续拼接坏帧。
            if ((Camera_RTOS_RecoverUartInputIfNeeded() != 0U) ||
                (Camera_RTOS_DiscardOverflowedInput() != 0U))
            {
                break;  // 退出有限字节循环，下轮从已复位的接收链路重新同步
            }

            // dispatcher 用当前 tick 维护二进制半帧 timeout，并输出至多一个业务事件。
            dispatch_result = CameraUartDispatcher_FeedByte(
                &s_camera_uart_dispatcher,
                rx_chunk[i],
                HAL_GetTick(),
                &dispatch_event);
            // parser 只生成命令或错误事件；业务由独立 ControlTask 从队列取出。
            Camera_RTOS_ProcessDispatchEvent(dispatch_result, &dispatch_event);
        }

        // 有持续 UART 流量时不会进入 received=0 分支，因此在 chunk 处理后重复同一周期采样。
        current_tick = HAL_GetTick();
        if ((current_tick - stack_sample_tick) >=
            CAMERA_RTOS_STACK_SAMPLE_PERIOD_MS)
        {
            Camera_RTOS_UpdateCommStackStats();
            stack_sample_tick = current_tick;
        }
    }
}

// ControlTask 是 CommandQueue 唯一 consumer，串行拥有图像处理和 SD 业务，并同步请求 CaptureTask。
// 队列为空时 Camera_CommandReceive() 使用 portMAX_DELAY，使任务保持 Blocked 而不轮询。
void Camera_RTOS_ControlTask(void *argument)
{
    CameraCommand_t command; // Queue 按值复制的稳定命令对象，不含 parser buffer 指针

    (void)argument; // CubeMX 任务入口保留统一参数，本任务当前不需要启动参数

    Camera_RTOS_UpdateControlStackStats();
    for (;;)
    {
        // 进入阻塞等待前发布心跳；waiting 标志防止正常 Blocked 被 IWDG 误判为卡死。
        s_camera_rtos_stats.control_heartbeat_count++;
        s_camera_rtos_stats.control_heartbeat_ms = HAL_GetTick();
        s_camera_control_waiting_for_command = 1U;

        if (Camera_CommandReceive(&command) == false)
        {
            // Queue 在任务创建前已初始化；失败表示内部状态错误，进入现有 assert 机制。
            s_camera_control_waiting_for_command = 0U;
            s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERR_COMMAND_QUEUE;
            configASSERT(0);
            (void)osDelay(1000U);
            continue;
        }

        // 收到命令后立即离开 waiting 状态并更新时间，使执行超时可被 MonitorTask 识别。
        s_camera_control_waiting_for_command = 0U;
        s_camera_rtos_stats.control_heartbeat_count++;
        s_camera_rtos_stats.control_heartbeat_ms = HAL_GetTick();
        Camera_RTOS_UpdateControlStackStats();

        Camera_RTOS_ExecuteCommand(&command);

        // 业务完成后再次发布心跳并采样最深调用链后的 stack high-water mark。
        s_camera_rtos_stats.control_heartbeat_count++;
        s_camera_rtos_stats.control_heartbeat_ms = HAL_GetTick();
        Camera_RTOS_UpdateControlStackStats();
    }
}

// MonitorTask 主循环：以 1000 ms 周期观察系统健康并独占 IWDG 刷新决策。
// 它不处理 UART 或图像，以免监控路径与 OV56RGB5 字节流、DCMI 状态产生资源竞争。
void Camera_RTOS_MonitorTask(void *argument)
{
    uint32_t current_tick;  // 本周期心跳、年龄判定和 IWDG 服务共用的 tick 快照

    (void)argument;  // CubeMX 任务入口保留统一参数，监控任务当前不需要启动参数

    // 监控任务与系统同生命周期，因此循环不退出；每轮首先 osDelay(1000) 阻塞，
    // 不会形成空转。延时返回后依次更新时间、心跳、资源余量和 IWDG 决策。
    for (;;)
    {
        (void)osDelay(1000U);
        s_camera_rtos_stats.uptime_ms += 1000U;

        // 先发布 MonitorTask 自身心跳，再用同一 tick 计算两个任务的年龄。
        s_camera_rtos_stats.monitor_heartbeat_count++;
        current_tick = HAL_GetTick();
        s_camera_rtos_stats.monitor_heartbeat_ms = current_tick;
        Camera_RTOS_UpdateMonitorHealthStats();
        Camera_RTOS_ServiceIwdg(current_tick);
    }
}

// 返回内部统计结构的只读视图，主要供 STATUS 命令读取缓存健康信息。
// 调用时只刷新心跳年龄，不主动操作 UART、DCMI、SDIO 或 FatFs，因此 STATUS 保持只读。
// 结构不会整体复制或加锁，各 volatile 字段可能来自相邻监控周期，适合诊断而非业务同步。
const CameraRtosStats_t *Camera_RTOS_GetStats(void)
{
    uint32_t current_tick = HAL_GetTick();  // 两个心跳年龄使用同一时间基准

    // 读取 STATUS 前即时更新年龄；tick 异常回退时 helper 按 0 处理，避免无符号下溢。
    Camera_RTOS_UpdateHeartbeatAges(current_tick);

    return &s_camera_rtos_stats;
}
