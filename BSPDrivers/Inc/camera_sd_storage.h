#ifndef ISP_OV5640_CAMERA_SD_STORAGE_H
#define ISP_OV5640_CAMERA_SD_STORAGE_H

#include <stdint.h>

#ifndef CAMERA_SD_INIT_CLOCK_DIV
#define CAMERA_SD_INIT_CLOCK_DIV (118U)
#endif

#define CAMERA_SD_OK                              0U
#define CAMERA_SD_ERR_NOT_IMPLEMENTED             1U
#define CAMERA_SD_ERR_NEED_TAKEOVER               3U
#define CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE     8U
#define CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED         9U
#define CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED 11U
#define CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED 12U
#define CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED     14U
#define CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED    15U
#define CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED  16U
#define CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED 17U
#define CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED        18U
#define CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED      19U
#define CAMERA_SD_ERR_CARD_INFO_FAILED            20U
#define CAMERA_SD_ERR_SENSOR_REG_READ_FAILED      27U
#define CAMERA_SD_ERR_SENSOR_REG_WRITE_FAILED     28U
#define CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED    29U
#define CAMERA_SD_ERR_NO_SAVED_3018               30U

typedef struct
{
    uint32_t supported;
    uint32_t card_ready;
    uint32_t takeover_required;
    uint32_t sdio_ready;
    uint32_t fatfs_ready;
    uint32_t last_error_code;
    const char *last_error_text;
    uint32_t dvp_mask_available;
    uint32_t dvp_mask_active;
    uint32_t dvp_reg_3018_saved;
    uint32_t dvp_reg_3018_current_or_restored;
    uint32_t last_sd_init_status;
    uint32_t last_sd_init_error;
    uint32_t last_sd_rw_status;
    uint32_t last_sd_rw_error;
} CameraSdStorageStatus_t;

void Camera_SDStorage_InitState(void);
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status);

/* Internal snapshot flow primitives; these are no longer exposed through CLI. */
uint32_t Camera_SDStorage_StopDvpConflictPads(void);
uint32_t Camera_SDStorage_RestoreDvpConflictPads(void);
uint32_t Camera_SDStorage_RequestTakeoverEnter(void);
uint32_t Camera_SDStorage_RequestTakeoverExit(void);
uint32_t Camera_SDStorage_RequestInit(void);

const char *Camera_SDStorage_ErrorToString(uint32_t error_code);

#endif /* ISP_OV5640_CAMERA_SD_STORAGE_H */
