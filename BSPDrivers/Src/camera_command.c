#include "camera_command.h"  // CameraCommand_t、队列深度和公开提交接口

#include "FreeRTOS.h"  // 提供 FreeRTOS 基础类型和 0 tick 常量
#include "queue.h"     // 提供 xQueueCreate、xQueueSend 和 xQueueReceive

#include <stddef.h>  // 提供 NULL

// 队列句柄仅由本模块持有；parser 和业务模块必须通过公开 API 提交或接收命令。
static QueueHandle_t s_camera_command_queue;

// Stage 15A 只保留最小内部统计，不改变现有 STATUS 输出字段。
static CameraCommandStats_t s_camera_command_stats;

// 拒绝 NONE 和枚举范围外的值，避免无效对象占用有限队列槽位。
static bool Camera_CommandIsValidType(CameraCommandType_t type)
{
    return ((type > CAMERA_CMD_NONE) &&
            (type <= CAMERA_CMD_IMAGE_REQUEST));
}

// 使用当前工程已有的动态 FreeRTOS 对象风格创建一个小型命令队列。
bool Camera_CommandInit(void)
{
    if (s_camera_command_queue != NULL)
    {
        return true;
    }

    s_camera_command_stats.command_submit_count = 0U;
    s_camera_command_stats.command_execute_count = 0U;
    s_camera_command_stats.command_drop_count = 0U;
    s_camera_command_stats.last_submit_result =
        CAMERA_COMMAND_SUBMIT_NOT_INITIALIZED;

    s_camera_command_queue = xQueueCreate(
        CAMERA_COMMAND_QUEUE_DEPTH,
        sizeof(CameraCommand_t));

    if (s_camera_command_queue != NULL)
    {
        s_camera_command_stats.last_submit_result = CAMERA_COMMAND_SUBMIT_OK;
    }

    return (s_camera_command_queue != NULL);
}

// parser 使用 0 tick 提交，队列满时明确计数和记录错误，不阻塞 UART 接收路径。
bool Camera_CommandSubmit(const CameraCommand_t *command)
{
    if (command == NULL)
    {
        s_camera_command_stats.command_drop_count++;
        s_camera_command_stats.last_submit_result =
            CAMERA_COMMAND_SUBMIT_BAD_ARGUMENT;
        return false;
    }

    if (Camera_CommandIsValidType(command->type) == false)
    {
        s_camera_command_stats.command_drop_count++;
        s_camera_command_stats.last_submit_result =
            CAMERA_COMMAND_SUBMIT_BAD_ARGUMENT;
        return false;
    }

    if (s_camera_command_queue == NULL)
    {
        s_camera_command_stats.command_drop_count++;
        s_camera_command_stats.last_submit_result =
            CAMERA_COMMAND_SUBMIT_NOT_INITIALIZED;
        return false;
    }

    if (xQueueSend(s_camera_command_queue, command, 0U) != pdPASS)
    {
        s_camera_command_stats.command_drop_count++;
        s_camera_command_stats.last_submit_result =
            CAMERA_COMMAND_SUBMIT_QUEUE_FULL;
        return false;
    }

    s_camera_command_stats.command_submit_count++;
    s_camera_command_stats.last_submit_result = CAMERA_COMMAND_SUBMIT_OK;
    return true;
}

// ControlTask 使用 portMAX_DELAY 阻塞等待；成功返回即表示该对象进入统一执行分派。
bool Camera_CommandReceive(CameraCommand_t *command)
{
    if ((command == NULL) || (s_camera_command_queue == NULL))
    {
        return false;
    }

    if (xQueueReceive(
            s_camera_command_queue,
            command,
            portMAX_DELAY) != pdPASS)
    {
        return false;
    }

    s_camera_command_stats.command_execute_count++;
    return true;
}

const CameraCommandStats_t *Camera_CommandGetStats(void)
{
    return &s_camera_command_stats;
}
