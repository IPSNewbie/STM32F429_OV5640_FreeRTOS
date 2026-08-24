#include "camera_snapshot_control.h"  // takeover guard、状态缓存和阻止计数接口

#include "camera_dcmi_dma.h"           // 导出的 DCMI 句柄供 HAL_DCMI_Stop 使用
#include "stm32f4xx_hal.h"             // HAL DCMI 停止和毫秒 tick

#include <stddef.h>                     // 提供 NULL

//============================================================================
// @file    camera_snapshot_control.c
// @brief   SD SNAPSHOT 前的 camera 暂停和软件请求闸门
//
// camera_sd_storage 在 staging 完成后调用 RequestPrepare：先激活 guard，使文本 DUMP
// 和 binary request 不能再启动新采集，再通过 HAL_DCMI_Stop 暂停硬件。
// guard 是项目逻辑闸门，不是 FreeRTOS mutex；本模块不切 GPIO、不改 0x3018、
// 不初始化 SDIO，也不执行最终硬件恢复。上述工作由 SD cleanup 按固定顺序完成。
// RequestRestore 只解除 guard 并保留历史 deferred 返回语义，调用方会继续恢复硬件。
//============================================================================

/*
 * 本模块只管理软件保护状态并执行 DCMI 停止动作。
 * GPIO、0x3018、SDIO 和最终图像链路恢复由 camera_sd_storage 按既定清理顺序完成。
 */
static CameraSnapshotControlStatus_t s_snapshot_control_status;

// 初始化调用/成功/错误计数、DCMI 停止诊断、历史策略字段和 guard 阻止计数。
// 其中若干 required/ready 字段仅保留为状态诊断，不构成另一套硬件状态机。
void Camera_SnapshotControl_InitState(void)
{
    s_snapshot_control_status.prepare_attempt_count = 0U;
    s_snapshot_control_status.restore_attempt_count = 0U;
    s_snapshot_control_status.prepare_success_count = 0U;
    s_snapshot_control_status.restore_success_count = 0U;
    s_snapshot_control_status.control_error_count = 0U;
    s_snapshot_control_status.last_error_code = CAMERA_SNAPSHOT_OK;
    s_snapshot_control_status.last_operation_ms = 0U;
    s_snapshot_control_status.real_dcmi_stop_enabled = 1U;
    s_snapshot_control_status.dcmi_stop_attempt_count = 0U;
    s_snapshot_control_status.dcmi_stop_success_count = 0U;
    s_snapshot_control_status.dcmi_stop_error_count = 0U;
    s_snapshot_control_status.last_dcmi_stop_hal_status = (uint32_t)HAL_OK;
    s_snapshot_control_status.camera_control_state = CAMERA_SNAPSHOT_STATE_IDLE;
    s_snapshot_control_status.dcmi_stop_required = 1U;
    s_snapshot_control_status.dcmi_dma_stop_required = 1U;
    s_snapshot_control_status.conflict_pin_release_required = 1U;
    s_snapshot_control_status.camera_restore_required = 1U;
    s_snapshot_control_status.frame_buffer_required = 1U;
    s_snapshot_control_status.frame_buffer_ready = 0U;
    s_snapshot_control_status.software_guard_active = 0U;
    s_snapshot_control_status.dump_block_required = 0U;
    s_snapshot_control_status.dump_block_count = 0U;
    s_snapshot_control_status.binary_block_count = 0U;
}

// 复制快照控制缓存状态，不访问硬件
void Camera_SnapshotControl_GetStatus(CameraSnapshotControlStatus_t *status)
{
    if (status == NULL)
    {
        return;
    }

    *status = s_snapshot_control_status;
}

// 在 staging 已稳定、GPIO takeover 尚未开始时，先激活 guard 再停止 DCMI。
// 即使 HAL_DCMI_Stop 失败也故意保持 guard，防止新请求进入，并交由统一 cleanup 解锁。
uint32_t Camera_SnapshotControl_RequestPrepare(void)
{
    uint32_t start_ms = HAL_GetTick();  // 记录控制调用耗时供 SD STATUS 诊断
    HAL_StatusTypeDef hal_status;       // 真实 HAL_DCMI_Stop 返回状态
    uint32_t result;                    // 映射后的稳定 snapshot 控制错误码

    ++s_snapshot_control_status.prepare_attempt_count;
    s_snapshot_control_status.software_guard_active = 1U;
    s_snapshot_control_status.dump_block_required = 1U;
    ++s_snapshot_control_status.dcmi_stop_attempt_count;

    hal_status = HAL_DCMI_Stop(&g_camera_dcmi);
    s_snapshot_control_status.last_dcmi_stop_hal_status = (uint32_t)hal_status;

    if (hal_status == HAL_OK)
    {
        ++s_snapshot_control_status.dcmi_stop_success_count;
        ++s_snapshot_control_status.prepare_success_count;
        s_snapshot_control_status.camera_control_state =
            CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED;
        s_snapshot_control_status.last_error_code = CAMERA_SNAPSHOT_OK;
        result = CAMERA_SNAPSHOT_OK;
    }
    else
    {
        ++s_snapshot_control_status.dcmi_stop_error_count;
        ++s_snapshot_control_status.control_error_count;
        /* 真实停止动作已经失败，因此使用 ERROR；guard 保持激活以阻止新请求。 */
        s_snapshot_control_status.camera_control_state =
            CAMERA_SNAPSHOT_STATE_ERROR;
        s_snapshot_control_status.last_error_code =
            CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED;
        result = CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED;
    }

    s_snapshot_control_status.last_operation_ms = HAL_GetTick() - start_ms;

    return result;
}

// 解除软件 guard 并记录 RESTORE_DEFERRED；本函数不恢复 GPIO、0x3018 或 DCMI。
// 非零返回值是保留的历史诊断语义，camera_sd_storage 当前会继续执行外部恢复步骤。
uint32_t Camera_SnapshotControl_RequestRestore(void)
{
    uint32_t start_ms = HAL_GetTick();

    /*
     * 保留历史 deferred 状态和返回码，表示本模块本身不执行硬件恢复；
     * camera_sd_storage 随后会恢复 GPIO、DVP 和 DCMI，不能在此重复操作。
     */
    ++s_snapshot_control_status.restore_attempt_count;
    s_snapshot_control_status.software_guard_active = 0U;
    s_snapshot_control_status.dump_block_required = 0U;
    s_snapshot_control_status.camera_control_state =
        CAMERA_SNAPSHOT_STATE_RESTORE_DEFERRED;
    s_snapshot_control_status.last_error_code =
        CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED;
    s_snapshot_control_status.last_operation_ms = HAL_GetTick() - start_ms;

    return CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED;
}

// 供 Camera RTOS 的文本 DUMP 和 binary request 共用；guard 激活时两者都必须拒绝。
uint32_t Camera_SnapshotControl_IsDumpAllowed(void)
{
    return (s_snapshot_control_status.software_guard_active == 0U) ? 1U : 0U;
}

// 查询快照软件保护是否激活
uint32_t Camera_SnapshotControl_IsSoftwareGuardActive(void)
{
    return s_snapshot_control_status.software_guard_active;
}

// 查询摄像头是否已进入快照暂停状态
uint32_t Camera_SnapshotControl_IsCameraPausedForSnapshot(void)
{
    return (s_snapshot_control_status.camera_control_state ==
            CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED) ? 1U : 0U;
}

// 仅读取 paused、guard active 和 block required 三个软件条件，不访问任何硬件。
uint32_t Camera_SnapshotControl_IsTakeoverPreconditionReady(void)
{
    /* 只读取软件状态，不访问或修改 DCMI、DMA、GPIO、SDIO。 */
    return ((Camera_SnapshotControl_IsCameraPausedForSnapshot() != 0U) &&
            (s_snapshot_control_status.software_guard_active == 1U) &&
            (s_snapshot_control_status.dump_block_required == 1U)) ? 1U : 0U;
}

// 只累计一次被 guard 拒绝的文本 DUMP，不发送 UART，也不操作 camera。
void Camera_SnapshotControl_RecordDumpBlocked(void)
{
    ++s_snapshot_control_status.dump_block_count;
}

// 只累计一次被 guard 拒绝的 binary request，不改变协议 parser。
void Camera_SnapshotControl_RecordBinaryBlocked(void)
{
    ++s_snapshot_control_status.binary_block_count;
}

// 将快照控制错误码转换为稳定文本
const char *Camera_SnapshotControl_ErrorToString(uint32_t error_code)
{
    switch (error_code)
    {
        case CAMERA_SNAPSHOT_OK:
            return "OK";

        case CAMERA_SNAPSHOT_ERR_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";

        case CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED:
            return "CAMERA_STOP_NOT_IMPLEMENTED";

        case CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED:
            return "CAMERA_RESTORE_NOT_IMPLEMENTED";

        case CAMERA_SNAPSHOT_ERR_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";

        case CAMERA_SNAPSHOT_ERR_INVALID_STATE:
            return "INVALID_STATE";

        case CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED:
            return "DCMI_STOP_FAILED";

        default:
            return "UNKNOWN_ERROR";
    }
}

// 将快照控制状态转换为稳定文本
const char *Camera_SnapshotControl_StateToString(uint32_t state)
{
    switch (state)
    {
        case CAMERA_SNAPSHOT_STATE_IDLE:
            return "IDLE";

        case CAMERA_SNAPSHOT_STATE_PREPARE_DEFERRED:
            return "PREPARE_DEFERRED";

        case CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED:
            return "CAMERA_PAUSED";

        case CAMERA_SNAPSHOT_STATE_RESTORE_DEFERRED:
            return "RESTORE_DEFERRED";

        case CAMERA_SNAPSHOT_STATE_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}
