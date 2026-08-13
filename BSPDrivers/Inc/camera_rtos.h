#ifndef CAMERA_RTOS_H
#define CAMERA_RTOS_H

#include <stdint.h>          // 提供统计字段、错误码和超时参数使用的固定宽度整数类型
#include "stm32f4xx_hal.h"  // 提供 UART、IWDG 句柄及 HAL_StatusTypeDef

/**
 * @file camera_rtos.h
 * @brief 摄像头 FreeRTOS 服务任务、健康监控和统一 RGB565 帧准备接口
 *
 * CommTask 独占 UART RX、文本/binary parser 和命令提交；ControlTask 阻塞等待
 * CommandQueue 并串行执行 CLI、DUMP、SD SNAPSHOT 等既有业务；MonitorTask 独立
 * 采集健康数据并决定是否刷新 IWDG。CaptureTask 独占原始帧采集；没有 Process/Storage Task。
 *
 * DUMP 与 SD SNAPSHOT 共用 @ref Camera_RTOS_PrepareRgb565Frame，确保二者都经过
 * DCMI 快照、back→front 提交及 BYPASS/GRAY/BINARY 处理。DCMI 帧完成 ISR 通过 Task
 * Notification 唤醒 CaptureTask；原始帧完成后，提交和图像处理仍留在 ControlTask。
 */

#ifndef CAMERA_SD_DIAG_SD_ONLY_BOOT
/**
 * @brief SD-only 历史诊断启动开关，正式配置保持关闭
 * @note 非零时会在没有摄像头图像链路的诊断构建中阻止 DUMP；不是运行期 CLI 选项。
 */
#define CAMERA_SD_DIAG_SD_ONLY_BOOT (0U)
#endif

/**
 * @brief DUMP 和 SD SNAPSHOT 等待一帧 DCMI 快照完成的默认上限，单位 ms
 * @note 超时后准备流程会停止 DCMI 并返回 CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT，
 *       防止摄像头异常时永久占住 ControlTask。
 */
#define CAMERA_RTOS_RGB565_PREPARE_TIMEOUT_MS (3000U)

//============================================================================
// 对外错误码、健康统计结构和任务入口
//============================================================================

//============================================================================
// 枚举：RTOS 运行错误码
//============================================================================

/**
 * @brief CommTask / ControlTask 公共运行错误码
 *
 * 简单状态使用独立值；采集启动、双缓冲提交、图像处理和 UART 发送使用分阶段
 * 基础码，实际返回值可将底层子错误码合并到低位，便于 STATUS 定位失败阶段。
 * @note 错误码只改变诊断可见状态，不改变 DUMP、binary request 或 SD 协议字段。
 */
typedef enum
{
    CAMERA_RTOS_ERR_NONE = 0x00000000U,                /**< 无错误 */
    CAMERA_RTOS_ERR_UART_NULL = 0x00000001U,           /**< UART 句柄为空 */
    CAMERA_RTOS_ERR_DUMP_FAILED = 0x00000002U,         /**< DUMP 失败 */
    CAMERA_RTOS_ERR_BAD_STATE = 0x00000003U,           /**< 运行状态异常 */
    CAMERA_RTOS_ERR_UNKNOWN_CMD = 0x00000004U,         /**< 未知 CLI 命令 */
    CAMERA_RTOS_ERR_UART_DMA_INIT = 0x00000005U,       /**< UART DMA 初始化失败 */
    CAMERA_RTOS_ERR_UART_DMA_RECOVERY = 0x00000006U,   /**< UART DMA 需要恢复 */
    CAMERA_RTOS_ERR_STREAM_OVERFLOW = 0x00000007U,     /**< StreamBuffer 溢出 */
    CAMERA_RTOS_ERR_SNAPSHOT_GUARD_ACTIVE = 0x00000008U, /**< SNAPSHOT 软件保护阻止图像请求 */
    CAMERA_RTOS_ERR_SD_ONLY_BOOT_NO_CAMERA = 0x00000009U, /**< SD-only 启动模式无相机图像链路 */
    CAMERA_RTOS_ERR_COMMAND_QUEUE = 0x0000000AU,       /**< CommandQueue 创建或提交失败 */
    CAMERA_RTOS_ERR_SNAPSHOT_START_BASE = 0x00000100U, /**< 快照启动失败基础码 */
    CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT = 0x00000200U,    /**< 快照等待超时 */
    CAMERA_RTOS_ERR_CAPTURE_COMMIT_BASE = 0x00000300U, /**< 帧缓冲提交失败基础码 */
    CAMERA_RTOS_ERR_IMAGE_PROCESS_BASE = 0x00000400U,  /**< 图像处理失败基础码 */
    CAMERA_RTOS_ERR_DUMP_SEND_BASE = 0x00000500U,      /**< 图像发送失败基础码 */
    CAMERA_RTOS_ERR_CAPTURE_HAL = 0x00000600U          /**< CaptureTask observed a DCMI/DMA HAL error */
} CameraRtosErrorCode_t;

/**
 * @brief MonitorTask 本周期不刷新 IWDG 的原因
 *
 * MonitorTask 会先检查 Hook 故障，再确认三个任务已经启动且活动任务心跳未超限。
 * 任何一项不健康时都记录具体原因并停止喂狗，让独立看门狗最终复位系统。
 */
typedef enum
{
    CAMERA_RTOS_IWDG_SKIP_NONE = 0U,                /**< 当前满足刷新条件 */
    CAMERA_RTOS_IWDG_SKIP_COMM_NOT_STARTED = 1U,    /**< CommTask 尚未启动 */
    CAMERA_RTOS_IWDG_SKIP_MONITOR_NOT_STARTED = 2U, /**< 监控任务尚未启动 */
    CAMERA_RTOS_IWDG_SKIP_COMM_TIMEOUT = 3U,        /**< CommTask 心跳超时 */
    CAMERA_RTOS_IWDG_SKIP_MONITOR_TIMEOUT = 4U,     /**< 监控任务心跳超时 */
    CAMERA_RTOS_IWDG_SKIP_HOOK_FAULT = 5U,          /**< FreeRTOS 保护 Hook 已报错 */
    CAMERA_RTOS_IWDG_SKIP_CONTROL_NOT_STARTED = 6U, /**< ControlTask 尚未启动 */
    CAMERA_RTOS_IWDG_SKIP_CONTROL_TIMEOUT = 7U      /**< ControlTask 执行业务时心跳超时 */
} CameraRtosIwdgSkipReason_t;

//============================================================================
// 结构体：RTOS 运行统计信息
//============================================================================

/**
 * @brief CommTask、ControlTask、MonitorTask 与 FreeRTOS Hook 共享的健康快照
 *
 * CommTask 更新 UART/parser 与通信心跳；ControlTask 更新业务、图像请求与控制心跳；
 * MonitorTask 更新 uptime、Heap、监控心跳及 IWDG 字段；严重 Hook 写 fault 字段。
 * 字段使用 volatile，确保各上下文每次都实际访问内存，但整个结构并不是一次性
 * 原子快照，STATUS 读取过程中个别计数继续变化属于允许的监控现象。
 */
typedef struct
{
    volatile uint32_t dump_request_count;        /**< 文本和二进制图像请求进入公共 DUMP 路径的总次数 */
    volatile uint32_t dump_success_count;        /**< 完成准备并成功发送 OV56RGB5 帧的次数 */
    volatile uint32_t dump_error_count;          /**< 被保护阻止或在准备、发送阶段失败的次数 */
    volatile uint32_t uart_none_count;           /**< UART DMA 每次限时读取没有取得字节的次数 */
    volatile uint32_t uart_pending_count;        /**< 文本解析器仍在等待行结束符的字节事件次数 */
    volatile uint32_t comm_rx_count;             /**< CommTask 从 StreamBuffer 取得的累计字节数 */
    volatile uint32_t comm_parse_error_count;    /**< CommTask 观察到的 parser/dispatcher 错误总数 */
    volatile uint32_t binary_request_count;      /**< 通过 magic、版本、长度、CRC 等校验的二进制请求次数 */
    volatile uint32_t binary_request_success_count; /**< 二进制请求成功发送图像次数 */
    volatile uint32_t binary_request_error_count;   /**< 二进制请求解析错误总数，下面各字段进一步分类 */
    volatile uint32_t binary_request_crc_error_count; /**< 二进制请求 CRC 错误次数 */
    volatile uint32_t binary_request_version_error_count; /**< 二进制请求版本错误次数 */
    volatile uint32_t binary_request_type_error_count; /**< 二进制请求类型错误次数 */
    volatile uint32_t binary_request_length_error_count; /**< 二进制请求长度错误次数 */
    volatile uint32_t binary_request_eof_error_count; /**< 二进制请求帧尾错误次数 */
    volatile uint32_t binary_request_timeout_count; /**< 二进制半帧超时次数 */
    volatile uint16_t last_binary_request_seq;  /**< 最近一次合法请求的协议 sequence，仅用于请求关联与诊断 */
    volatile uint32_t last_binary_error_code;   /**< 最近一次二进制解析失败的 ImageRequestParseResult_t 值 */
    volatile uint32_t last_error_code;           /**< Comm/Control 公共运行错误码 */
    volatile uint32_t last_dump_time_ms;         /**< 最近一次公共 DUMP 执行路径耗时，单位 ms */
    volatile uint32_t uptime_ms;                 /**< MonitorTask 按 1000 ms 周期累计的运行时间 */
    volatile uint32_t comm_stack_min_free_bytes;  /**< CommTask 历史最小剩余栈，单位 B */
    volatile uint32_t control_stack_min_free_bytes; /**< ControlTask 历史最小剩余栈，单位 B */
    volatile uint32_t capture_stack_min_free_bytes; /**< CaptureTask stack high-water mark in bytes */
    volatile uint32_t monitor_stack_min_free_bytes; /**< MonitorTask 历史最小剩余栈，即 stack high-water mark，单位 B */
    volatile uint32_t free_heap_bytes;            /**< 当前 FreeRTOS Heap 余量，单位 B */
    volatile uint32_t min_ever_free_heap_bytes;   /**< FreeRTOS 历史最小 Heap 余量，单位 B */
    volatile uint32_t hook_fault_code;             /**< 最近一次栈溢出、分配失败或断言 Hook 的故障类型 */
    volatile uint32_t hook_fault_count;            /**< FreeRTOS 保护 Hook 累计触发次数 */
    volatile uint32_t assert_line;                 /**< 最近一次 configASSERT 失败行号，其他 Hook 写 0 */
    volatile uint32_t comm_heartbeat_count;        /**< CommTask 每轮 UART 服务循环的心跳计数 */
    volatile uint32_t control_heartbeat_count;     /**< ControlTask 等待及执行命令边界的心跳计数 */
    volatile uint32_t monitor_heartbeat_count;     /**< MonitorTask 每个 1000 ms 周期递增的活跃计数 */
    volatile uint32_t comm_heartbeat_ms;           /**< CommTask 最近心跳 tick，单位 ms */
    volatile uint32_t control_heartbeat_ms;        /**< ControlTask 最近心跳 tick，单位 ms */
    volatile uint32_t monitor_heartbeat_ms;        /**< MonitorTask 最近一次健康采样的 tick，单位 ms */
    volatile uint32_t comm_heartbeat_age_ms;       /**< 当前 tick 距 CommTask 最近心跳的时间 */
    volatile uint32_t control_heartbeat_age_ms;    /**< 当前 tick 距 ControlTask 最近心跳的时间 */
    volatile uint32_t monitor_heartbeat_age_ms;    /**< 当前 tick 距 MonitorTask 最近心跳的时间 */
    volatile uint32_t iwdg_enabled;                /**< IWDG 初始化成功后置 1，之后硬件无法由软件关闭 */
    volatile uint32_t iwdg_refresh_skip_count;     /**< 健康条件不满足而主动跳过喂狗的累计次数 */
    volatile uint32_t iwdg_last_skip_reason;       /**< 最近一次 CameraRtosIwdgSkipReason_t 原因 */
} CameraRtosStats_t;

//============================================================================
// RTOS 接口函数
//============================================================================

/**
 * @brief 初始化摄像头 RTOS 运行状态并登记 USART1 句柄
 * @param huart CommTask 接收请求、ControlTask 发送回复所用的 UART 句柄
 * @return 无
 * @note 应在调度器启动前调用。函数会重置统计与 OV56RGB5 frame_id；若 huart 为
 *       NULL，则记录 CAMERA_RTOS_ERR_UART_NULL，CommTask 会按周期等待有效句柄。
 */
void Camera_RTOS_Init(UART_HandleTypeDef *huart);

/**
 * @brief 按当前约 8 秒窗口配置并启动独立看门狗 IWDG
 * @return HAL_OK-寄存器更新完成且 IWDG 已启动；HAL_ERROR-参数/实例异常；
 *         HAL_TIMEOUT-IWDG 更新状态在限定时间内未就绪
 * @note 应在任务创建完成后、调度器启动前调用。启动后只有 MonitorTask 会根据
 *       CommTask、ControlTask、MonitorTask 心跳和 Hook 故障状态决定是否刷新。
 */
HAL_StatusTypeDef Camera_RTOS_IwdgInit(void);

/**
 * @brief 准备一帧可供 DUMP 或 SD SNAPSHOT 使用的 RGB565 图像
 * @param timeout_ms 等待 DCMI 快照完成的最大时间，单位 ms
 * @return CAMERA_RTOS_ERR_NONE-成功，其他值-采集、提交、处理失败或超时
 * @note 仅在 ControlTask 上下文同步调用。CommTask 只解析并提交 DUMP、binary request
 *       和 SD SNAPSHOT；ControlTask 出队后进入该接口，再同步请求 CaptureTask 独占 DCMI/DMA。
 *       本接口限时等待并让出 CPU，完成后按 back→front→处理后再次 commit 的路径
 *       发布最终稳定帧。它不负责 UART 发送，也不负责 SDIO takeover 或文件写入。
 */
uint32_t Camera_RTOS_PrepareRgb565Frame(uint32_t timeout_ms);

/**
 * @brief 记录一次进入公共 DUMP 执行路径的图像请求
 * @return 无
 * @note 文本 DUMP 与合法 binary image request 都会计入该总数。
 */
void Camera_RTOS_RecordDumpRequest(void);

/**
 * @brief 记录一次成功发送的 DUMP 及其总耗时
 * @param elapsed_ms 从开始准备图像到 OV56RGB5 帧发送完成的时间，单位 ms
 * @return 无
 */
void Camera_RTOS_RecordDumpSuccess(uint32_t elapsed_ms);

/**
 * @brief 记录一次被阻止或执行失败的 DUMP
 * @param error_code 本次失败的 CameraRtosErrorCode_t 或带底层子码的组合值
 * @return 无
 * @note 传入 CAMERA_RTOS_ERR_NONE 时会改记为 CAMERA_RTOS_ERR_DUMP_FAILED，
 *       避免失败计数增加却把“最近错误”显示为成功。
 */
void Camera_RTOS_RecordDumpError(uint32_t error_code);

/**
 * @brief 记录 CommTask 一次限时 UART 读取没有取得数据
 * @return 无
 * @note 这是正常空闲诊断计数，不等同于 UART 硬件错误。
 */
void Camera_RTOS_RecordUartNone(void);

/**
 * @brief 记录文本命令解析器仍在等待行结束符的状态
 * @return 无
 * @note 用于观察命令流是否长期停留在半行，不表示 binary request 的半帧状态。
 */
void Camera_RTOS_RecordUartPending(void);

/**
 * @brief 记录 FreeRTOS 严重错误 Hook，供 MonitorTask 停止刷新 IWDG
 * @param fault_code 故障类型：1-栈溢出，2-内存分配失败，3-断言失败
 * @param assert_line configASSERT 失败行号；其他 Hook 传 0
 * @return 无
 * @note 该接口可能处在系统已不健康的 Hook 上下文，因此只写静态字段，不分配
 *       内存、不等待 RTOS 对象，也不直接执行复杂恢复。MonitorTask 后续看到故障码
 *       后会停止喂狗，由 IWDG 完成硬件复位。
 */
void Camera_RTOS_RecordHookFault(uint32_t fault_code, uint32_t assert_line);

/**
 * @brief UART 通信输入与 parser 任务
 * @param argument 任务参数，当前未使用
 * @return 不返回
 * @note 只消费 StreamBuffer、维护文本/binary parser、生成并提交 CameraCommand_t，
 *       不执行 DUMP、SD、PROC/THR/RESET、DCMI 或图像处理业务。
 */
void Camera_RTOS_CommTask(void *argument);

/**
 * @brief CommandQueue 唯一业务消费者任务
 * @param argument 任务参数，当前未使用
 * @return 不返回
 * @note 使用 portMAX_DELAY 阻塞等待命令，串行执行现有 CLI、DUMP、SD SNAPSHOT 和
 *       binary image request；Capture/Process/Storage 职责仍暂留本任务。
 */
void Camera_RTOS_ControlTask(void *argument);

/**
 * @brief 周期采样任务、Heap 健康状态并有条件刷新 IWDG
 * @param argument 任务参数，当前未使用
 * @return 不返回
 * @note 每轮先阻塞 1000 ms，再更新 uptime、MonitorTask 心跳、历史最小栈余量和
 *       Heap 余量。只有本任务可以喂狗，这样 Comm/Control 卡死时不能通过
 *       自己的代码继续刷新 IWDG；本任务不使用 UART，避免与图像输出字节流竞争。
 */
void Camera_RTOS_MonitorTask(void *argument);

/**
 * @brief 获取内部运行统计的只读视图
 * @return 指向内部静态 @ref CameraRtosStats_t 结构体的指针
 * @note 返回前会按当前 tick 更新三个任务的心跳年龄。函数不复制整个结构，因此
 *       STATUS 读取多个 volatile 字段时 MonitorTask 可能继续更新其中少数字段；
 *       调用者不得修改或释放返回的指针。
 */
const CameraRtosStats_t *Camera_RTOS_GetStats(void);

#endif /* CAMERA_RTOS_H */
