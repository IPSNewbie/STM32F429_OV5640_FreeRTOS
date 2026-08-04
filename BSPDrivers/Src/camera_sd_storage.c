#include "camera_sd_storage.h"

#include "stm32f4xx_hal.h"

#include <stddef.h>

/*
 * Stage 11B-1 仅保存软件状态。
 * PC8、PC9、PC11 同时被 DCMI 和 SDIO 使用，后续必须先停止 DCMI 和相关 DMA，
 * 再进入 SDIO 接管流程；本文件不切换引脚，也不初始化 SDIO 或 FATFS。
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

        default:
            return "UNKNOWN_ERROR";
    }
}
