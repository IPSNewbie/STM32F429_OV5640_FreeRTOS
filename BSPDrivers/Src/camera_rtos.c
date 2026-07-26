#include "camera_rtos.h"

#include "camera_cli.h"
#include "camera_dcmi_dma.h"
#include "camera_frame_buffer.h"
#include "camera_image_process.h"
#include "camera_pc_dump.h"
#include "cmsis_os.h"

#include <stddef.h>

//============================================================================
// @file    camera_rtos.c
// @brief   摄像头 RTOS 任务模块实现
//          提供 CameraServiceTask（处理 PC 命令、拍照、图像处理、发送帧）
//          和 MonitorTask（定期统计监控）两个 FreeRTOS 任务。
//============================================================================

// 等待 DCMI 快照完成的最大超时时间（毫秒）
#define CAMERA_RTOS_SNAPSHOT_TIMEOUT_MS          3000U

// 错误码定义（无错误）
#define CAMERA_RTOS_ERROR_NONE                   0x00000000U

// UART 句柄为空
#define CAMERA_RTOS_ERROR_UART_NULL              0x00000001U

// 快照启动失败的基础错误码（低 8 位为子错误码）
#define CAMERA_RTOS_ERROR_SNAPSHOT_START_BASE    0x00000100U

// 快照超时错误
#define CAMERA_RTOS_ERROR_SNAPSHOT_TIMEOUT       0x00000200U

// 提交后台缓冲区失败的基础错误码
#define CAMERA_RTOS_ERROR_CAPTURE_COMMIT_BASE    0x00000300U

// 图像处理失败的基础错误码
#define CAMERA_RTOS_ERROR_IMAGE_PROCESS_BASE     0x00000400U

// PC Dump 发送失败的基础错误码
#define CAMERA_RTOS_ERROR_DUMP_SEND_BASE         0x00000500U

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
    s_camera_rtos_stats.dump_success_count = 0U;
    s_camera_rtos_stats.dump_error_count = 0U;
    s_camera_rtos_stats.monitor_tick_count = 0U;
    s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERROR_NONE;
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
        return CAMERA_RTOS_ERROR_SNAPSHOT_START_BASE | (uint32_t)snapshot_ret;
    }

    // 等待快照完成，超时则停止并返回错误
    snapshot_start_tick = HAL_GetTick();
    while (Camera_DCMI_IsSnapshotDone() == 0U)
    {
        if ((HAL_GetTick() - snapshot_start_tick) > CAMERA_RTOS_SNAPSHOT_TIMEOUT_MS)
        {
            Camera_DCMI_Stop();
            return CAMERA_RTOS_ERROR_SNAPSHOT_TIMEOUT;
        }
        (void)osDelay(1U);  // 让出 CPU，避免忙等
    }

    Camera_DCMI_Stop();

    // 提交后台缓冲区，使新捕获的帧成为前台帧
    commit_ret = Camera_FrameBuffer_CommitBackBuffer();
    if (commit_ret != CAMERA_FB_OK)
    {
        return CAMERA_RTOS_ERROR_CAPTURE_COMMIT_BASE | (uint32_t)commit_ret;
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
            return CAMERA_RTOS_ERROR_IMAGE_PROCESS_BASE | (uint32_t)process_ret;
        }
    }

    // 将当前前台帧通过 UART 发送给 PC
    dump_ret = Camera_PC_Dump_SendFrame(s_camera_rtos_uart, s_camera_rtos_frame_id);
    if (dump_ret != 0U)
    {
        return CAMERA_RTOS_ERROR_DUMP_SEND_BASE | (uint32_t)dump_ret;
    }

    // 成功发送后递增帧序号
    s_camera_rtos_frame_id++;
    return CAMERA_RTOS_ERROR_NONE;
}

// 初始化摄像头 RTOS 模块（保存 UART 句柄，重置统计和帧号）
void Camera_RTOS_Init(UART_HandleTypeDef *huart)
{
    s_camera_rtos_uart = NULL;
    s_camera_rtos_frame_id = 1U;
    Camera_RTOS_ClearStats();

    if (huart == NULL)
    {
        s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERROR_UART_NULL;
        return;
    }

    s_camera_rtos_uart = huart;
}

// 摄像头服务任务（FreeRTOS 任务主循环）
void Camera_RTOS_CameraServiceTask(void *argument)
{
    uint8_t command;
    uint32_t error_code;

    (void)argument;  // 未使用的参数

    for (;;)
    {
        // 如果 UART 句柄无效，记录错误并延时
        if (s_camera_rtos_uart == NULL)
        {
            s_camera_rtos_stats.last_error_code = CAMERA_RTOS_ERROR_UART_NULL;
            (void)osDelay(1U);
            continue;
        }

        // 轮询 PC 命令（非阻塞）
        command = Camera_PC_Dump_PollCommand(s_camera_rtos_uart);
        if (command == CAMERA_PC_DUMP_CMD_PENDING)
        {
            /*
             * 已经收到过有效字节，但命令尚未完整。
             * 这里不能 osDelay，否则 HELP\r\n 这种连续字节会丢失。
             */
            continue;
        }


        if (command == CAMERA_PC_DUMP_CMD_NONE)
        {
            /*
             * 本轮没有收到任何字节，才允许让出 CPU。
             */
            (void)osDelay(1U);
            continue;
        }

        // 每处理一个有效命令，循环计数加 1
        s_camera_rtos_stats.camera_service_loop_count++;

        // 如果命令不是 DUMP（例如 AEC 或 CLI 内部命令），则忽略并继续
        if (command != CAMERA_PC_DUMP_CMD_DUMP)
        {
            continue;
        }

        // 执行 DUMP 流程（捕获、处理、发送）
        error_code = Camera_RTOS_CaptureProcessAndSend();
        if (error_code == CAMERA_RTOS_ERROR_NONE)
        {
            s_camera_rtos_stats.dump_success_count++;
        }
        else
        {
            s_camera_rtos_stats.dump_error_count++;
            s_camera_rtos_stats.last_error_code = error_code;
        }
    }
}

// MonitorTask：每 1000 ms 更新心跳计数，不使用 UART。
void Camera_RTOS_MonitorTask(void *argument)
{
    (void)argument;

    for (;;)
    {
        (void)osDelay(1000U);          // 每秒运行一次
        s_camera_rtos_stats.monitor_tick_count++;  // 累计运行次数
    }
}

// 获取运行统计信息的只读指针
const CameraRtosStats_t *Camera_RTOS_GetStats(void)
{
    return &s_camera_rtos_stats;
}
