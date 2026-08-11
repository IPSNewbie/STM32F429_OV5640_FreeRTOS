#ifndef ISP_OV5640_CAMERA_COMMAND_H
#define ISP_OV5640_CAMERA_COMMAND_H

#include "camera_image_process.h"  // 提供 PROC 命令使用的 BYPASS/GRAY/BINARY 模式

#include <stdbool.h>  // 提供命令队列初始化、提交和接收接口的布尔结果
#include <stdint.h>   // 提供阈值、请求序号和统计计数的固定宽度类型

/** @brief CameraCommandQueue 固定深度；保持队列较小并限制最坏堆占用。 */
#define CAMERA_COMMAND_QUEUE_DEPTH (8U)

/**
 * @brief 当前固件实际支持的统一命令类型
 * @note 查询和设置使用不同类型，使队列中的值对象不依赖原始文本或 parser buffer。
 */
typedef enum
{
    CAMERA_CMD_NONE = 0,
    CAMERA_CMD_HELP,
    CAMERA_CMD_STATUS,
    CAMERA_CMD_CLI_ERROR,
    CAMERA_CMD_PROC_GET,
    CAMERA_CMD_PROC_SET,
    CAMERA_CMD_THRESHOLD_GET,
    CAMERA_CMD_THRESHOLD_SET,
    CAMERA_CMD_RESET,
    CAMERA_CMD_DUMP,
    CAMERA_CMD_SD_STATUS,
    CAMERA_CMD_SD_SNAPSHOT,
    CAMERA_CMD_IMAGE_REQUEST
} CameraCommandType_t;

/**
 * @brief 由 CommTask parser 识别、由 ControlTask 输出的现有 CLI 错误
 * @note 这些值只搬运既有错误语义，不增加新命令或新 UART 文本。
 */
typedef enum
{
    CAMERA_COMMAND_CLI_ERROR_UNKNOWN = 0,
    CAMERA_COMMAND_CLI_ERROR_BAD_PROC_ARG,
    CAMERA_COMMAND_CLI_ERROR_BAD_THRESHOLD_ARG
} CameraCommandCliError_t;

/**
 * @brief 可直接复制进 FreeRTOS Queue 的小型命令值对象
 * @note 结构中不保存字符串、parser buffer 或其他具有临时生命周期的指针。
 */
typedef struct
{
    CameraCommandType_t type; /**< 决定执行分支的命令类型 */
    union
    {
        struct
        {
            CameraProcessMode_t mode; /**< PROC SET 的目标处理模式 */
        } proc;
        struct
        {
            uint8_t threshold; /**< THR SET 的 0~255 阈值 */
        } threshold;
        struct
        {
            uint16_t seq; /**< binary image request 的 PC 请求序号 */
        } image_request;
        struct
        {
            CameraCommandCliError_t error; /**< parser 已确定的既有 CLI 错误 */
        } cli_error;
    } args;
} CameraCommand_t;

/** @brief 最近一次命令提交结果；队列满和未初始化都不会静默继续。 */
typedef enum
{
    CAMERA_COMMAND_SUBMIT_OK = 0,
    CAMERA_COMMAND_SUBMIT_BAD_ARGUMENT,
    CAMERA_COMMAND_SUBMIT_NOT_INITIALIZED,
    CAMERA_COMMAND_SUBMIT_QUEUE_FULL
} CameraCommandSubmitResult_t;

/**
 * @brief CommandQueue 内部运行统计
 * @note Stage 15A 不把这些字段加入 STATUS，避免改变现有 UART 文本接口。
 */
typedef struct
{
    volatile uint32_t command_submit_count;  /**< 成功复制进队列的命令数 */
    volatile uint32_t command_execute_count; /**< 成功出队并进入执行分派的命令数 */
    volatile uint32_t command_drop_count;    /**< 因参数、未初始化或队列满而丢弃的命令数 */
    volatile CameraCommandSubmitResult_t last_submit_result; /**< 最近提交结果 */
} CameraCommandStats_t;

/**
 * @brief 使用 xQueueCreate() 创建深度为 8 的 CameraCommand_t 队列
 * @return true-创建成功或已经创建；false-创建失败
 * @note 创建失败由 FreeRTOS 初始化入口进入现有 Error_Handler 故障机制。
 */
bool Camera_CommandInit(void);

/**
 * @brief 以 0 tick 等待把一个命令值复制进队列
 * @param command 待提交的完整命令值对象
 * @return true-提交成功；false-参数无效、队列未初始化或队列已满
 */
bool Camera_CommandSubmit(const CameraCommand_t *command);

/**
 * @brief 由 ControlTask 阻塞等待并取出一个待执行命令
 * @param command 接收命令副本的输出对象
 * @return true-成功取得命令；false-参数无效或队列未初始化
 * @note 内部使用 portMAX_DELAY；队列为空时 ControlTask 保持 Blocked，不周期轮询。
 */
bool Camera_CommandReceive(CameraCommand_t *command);

/** @brief 获取 CommandQueue 内部统计的只读实时视图。 */
const CameraCommandStats_t *Camera_CommandGetStats(void);

#endif /* ISP_OV5640_CAMERA_COMMAND_H */
