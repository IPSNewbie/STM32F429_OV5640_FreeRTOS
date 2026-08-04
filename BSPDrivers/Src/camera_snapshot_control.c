#include "camera_snapshot_control.h"

#include "stm32f4xx_hal.h"

#include <stddef.h>

/*
 * Stage 11B-5 增加拍照保存前后的软件保护状态。
 * 本模块不停止或恢复 DCMI/DMA，不释放冲突引脚，也不访问 SDIO 或 FATFS。
 */
static CameraSnapshotControlStatus_t s_snapshot_control_status;

void Camera_SnapshotControl_InitState(void)
{
    s_snapshot_control_status.prepare_attempt_count = 0U;
    s_snapshot_control_status.restore_attempt_count = 0U;
    s_snapshot_control_status.prepare_success_count = 0U;
    s_snapshot_control_status.restore_success_count = 0U;
    s_snapshot_control_status.control_error_count = 0U;
    s_snapshot_control_status.last_error_code = CAMERA_SNAPSHOT_OK;
    s_snapshot_control_status.last_operation_ms = 0U;
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

void Camera_SnapshotControl_GetStatus(CameraSnapshotControlStatus_t *status)
{
    if (status == NULL)
    {
        return;
    }

    *status = s_snapshot_control_status;
}

uint32_t Camera_SnapshotControl_RequestPrepare(void)
{
    uint32_t start_ms = HAL_GetTick();

    /* deferred 只表示硬件停止边界尚未实现，不计为真实硬件错误。 */
    ++s_snapshot_control_status.prepare_attempt_count;
    s_snapshot_control_status.software_guard_active = 1U;
    s_snapshot_control_status.dump_block_required = 1U;
    s_snapshot_control_status.camera_control_state =
        CAMERA_SNAPSHOT_STATE_PREPARE_DEFERRED;
    s_snapshot_control_status.last_error_code =
        CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED;
    s_snapshot_control_status.last_operation_ms = HAL_GetTick() - start_ms;

    return CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED;
}

uint32_t Camera_SnapshotControl_RequestRestore(void)
{
    uint32_t start_ms = HAL_GetTick();

    /* deferred 只表示硬件恢复边界尚未实现，不计为真实硬件错误。 */
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

uint32_t Camera_SnapshotControl_IsDumpAllowed(void)
{
    return (s_snapshot_control_status.software_guard_active == 0U) ? 1U : 0U;
}

uint32_t Camera_SnapshotControl_IsSoftwareGuardActive(void)
{
    return s_snapshot_control_status.software_guard_active;
}

void Camera_SnapshotControl_RecordDumpBlocked(void)
{
    ++s_snapshot_control_status.dump_block_count;
}

void Camera_SnapshotControl_RecordBinaryBlocked(void)
{
    ++s_snapshot_control_status.binary_block_count;
}

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

        default:
            return "UNKNOWN_ERROR";
    }
}

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
