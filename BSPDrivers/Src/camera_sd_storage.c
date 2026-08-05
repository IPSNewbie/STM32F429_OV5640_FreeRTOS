#include "camera_sd_storage.h"

#include "camera_snapshot_control.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>

/*
 * Stage 11B-8 在进入接管请求前检查 SNAPSHOT 暂停和软件保护状态。
 * PC8、PC9、PC11 同时被 DCMI 和 SDIO 使用，后续必须先停止 DCMI 和相关 DMA，
 * 再进入 SDIO 接管流程；本文件不停止 DCMI、不切换引脚，也不初始化 SDIO 或 FATFS。
 */
static CameraSdStorageStatus_t s_camera_sd_status;

void Camera_SDStorage_InitState(void)
{
    s_camera_sd_status.init_attempt_count = 0U;
    s_camera_sd_status.init_success_count = 0U;
    s_camera_sd_status.init_error_count = 0U;
    s_camera_sd_status.last_error_code = CAMERA_SD_OK;
    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_operation_ms = 0U;
    s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_IDLE;
    s_camera_sd_status.takeover_enter_attempt_count = 0U;
    s_camera_sd_status.takeover_exit_attempt_count = 0U;
    s_camera_sd_status.takeover_enter_success_count = 0U;
    s_camera_sd_status.takeover_exit_success_count = 0U;
    s_camera_sd_status.takeover_error_count = 0U;
    s_camera_sd_status.last_takeover_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_takeover_operation_ms = 0U;
    s_camera_sd_status.takeover_precheck_required = 1U;
    s_camera_sd_status.takeover_precheck_attempt_count = 0U;
    s_camera_sd_status.takeover_precheck_success_count = 0U;
    s_camera_sd_status.takeover_precheck_fail_count = 0U;
    s_camera_sd_status.snapshot_pause_required = 1U;
    s_camera_sd_status.snapshot_pause_confirmed = 0U;
    s_camera_sd_status.conflict_pin_release_ready = 0U;
    s_camera_sd_status.last_takeover_precheck_error_code = CAMERA_SD_OK;
}

void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status)
{
    if (status == NULL)
    {
        return;
    }

    *status = s_camera_sd_status;
}

uint32_t Camera_SDStorage_RequestInit(void)
{
    /* 只记录受控请求，不把“等待接管”统计为 SD 卡硬件初始化失败。 */
    ++s_camera_sd_status.init_attempt_count;
    s_camera_sd_status.last_error_code = CAMERA_SD_ERR_NEED_TAKEOVER;
    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_operation_ms = HAL_GetTick();

    return CAMERA_SD_ERR_NEED_TAKEOVER;
}

uint32_t Camera_SDStorage_RequestTakeoverEnter(void)
{
    uint32_t start_ms = HAL_GetTick();
    uint32_t result;

    ++s_camera_sd_status.takeover_enter_attempt_count;
    ++s_camera_sd_status.takeover_precheck_attempt_count;

    if (Camera_SnapshotControl_IsTakeoverPreconditionReady() == 0U)
    {
        ++s_camera_sd_status.takeover_precheck_fail_count;
        s_camera_sd_status.snapshot_pause_confirmed = 0U;
        s_camera_sd_status.conflict_pin_release_ready = 0U;
        /* 前置条件失败时保持 IDLE，避免误认为接管流程已经开始。 */
        s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_IDLE;
        s_camera_sd_status.last_takeover_error_code =
            CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
        s_camera_sd_status.last_takeover_precheck_error_code =
            CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
        result = CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
    }
    else
    {
        ++s_camera_sd_status.takeover_precheck_success_count;
        s_camera_sd_status.snapshot_pause_confirmed = 1U;
        s_camera_sd_status.conflict_pin_release_ready = 1U;
        /* 条件已满足，但本阶段仍不释放或切换冲突引脚。 */
        s_camera_sd_status.takeover_state =
            CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED;
        s_camera_sd_status.last_takeover_precheck_error_code = CAMERA_SD_OK;
        s_camera_sd_status.last_takeover_error_code =
            CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
        result = CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
    }

    s_camera_sd_status.last_takeover_operation_ms = HAL_GetTick() - start_ms;

    return result;
}

uint32_t Camera_SDStorage_RequestTakeoverExit(void)
{
    uint32_t start_ms = HAL_GetTick();

    /* 只记录退出请求，不恢复 DCMI/DMA，不释放或切换冲突引脚。 */
    ++s_camera_sd_status.takeover_exit_attempt_count;
    s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED;
    s_camera_sd_status.snapshot_pause_confirmed = 0U;
    s_camera_sd_status.conflict_pin_release_ready = 0U;
    s_camera_sd_status.last_takeover_error_code =
        CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
    s_camera_sd_status.last_takeover_operation_ms = HAL_GetTick() - start_ms;

    return CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
}

const char *Camera_SDStorage_ErrorToString(uint32_t error_code)
{
    switch (error_code)
    {
        case CAMERA_SD_OK:
            return "OK";

        case CAMERA_SD_ERR_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";

        case CAMERA_SD_ERR_PIN_CONFLICT:
            return "PIN_CONFLICT";

        case CAMERA_SD_ERR_NEED_TAKEOVER:
            return "NEED_TAKEOVER";

        case CAMERA_SD_ERR_INIT_FAILED:
            return "INIT_FAILED";

        case CAMERA_SD_ERR_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";

        case CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED:
            return "TAKEOVER_NOT_IMPLEMENTED";

        case CAMERA_SD_ERR_TAKEOVER_NOT_ACTIVE:
            return "TAKEOVER_NOT_ACTIVE";

        case CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE:
            return "TAKEOVER_ALREADY_ACTIVE";

        case CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED:
            return "SNAPSHOT_NOT_PAUSED";

        case CAMERA_SD_ERR_TAKEOVER_PRECHECK_FAILED:
            return "TAKEOVER_PRECHECK_FAILED";

        default:
            return "UNKNOWN_ERROR";
    }
}

const char *Camera_SDStorage_TakeoverStateToString(uint32_t state)
{
    switch (state)
    {
        case CAMERA_SD_TAKEOVER_STATE_IDLE:
            return "IDLE";

        case CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED:
            return "ENTER_DEFERRED";

        case CAMERA_SD_TAKEOVER_STATE_ACTIVE:
            return "ACTIVE";

        case CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED:
            return "EXIT_DEFERRED";

        case CAMERA_SD_TAKEOVER_STATE_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}
