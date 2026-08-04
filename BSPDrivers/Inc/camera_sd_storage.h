#ifndef ISP_OV5640_CAMERA_SD_STORAGE_H
#define ISP_OV5640_CAMERA_SD_STORAGE_H

#include <stdint.h>

/* SD 卡模块返回码。Stage 11B-1 只提供受控入口，不执行硬件初始化。 */
#define CAMERA_SD_OK                         0U
#define CAMERA_SD_ERR_NOT_IMPLEMENTED        1U
#define CAMERA_SD_ERR_PIN_CONFLICT           2U
#define CAMERA_SD_ERR_NEED_TAKEOVER          3U
#define CAMERA_SD_ERR_INIT_FAILED            4U
#define CAMERA_SD_ERR_INVALID_ARGUMENT       5U

/* SD 卡模块的软件状态，不包含 SDIO 或 FATFS 对象。 */
typedef struct
{
    uint32_t init_attempt_count; /* SD INIT 调用次数。 */
    uint32_t init_success_count; /* SD 卡初始化成功次数，本阶段保持为 0。 */
    uint32_t init_error_count;   /* 硬件初始化失败次数，本阶段不计入延后请求。 */
    uint32_t last_error_code;    /* 最近一次 SD INIT 请求的返回码。 */
    uint32_t is_initialized;     /* SD 卡是否已初始化，本阶段固定为 0。 */
    uint32_t takeover_required;  /* 是否需要停止 DCMI 并由 SDIO 接管冲突引脚。 */
    uint32_t sdio_ready;         /* SDIO 是否已就绪，本阶段固定为 0。 */
    uint32_t fatfs_ready;        /* FATFS 是否已挂载，本阶段固定为 0。 */
    uint32_t last_operation_ms;  /* 最近一次 SD INIT 请求的系统时间。 */
} CameraSdStorageStatus_t;

/* 初始化纯软件状态，不访问 SDIO、GPIO 或文件系统。 */
void Camera_SDStorage_InitState(void);

/* 将当前软件状态复制到调用者提供的结构体。 */
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status);

/* 记录一次初始化请求；当前返回 NEED_TAKEOVER，不执行真实初始化。 */
uint32_t Camera_SDStorage_RequestInit(void);

/* 将 SD 卡模块返回码转换为 CLI 可读文本。 */
const char *Camera_SDStorage_ErrorToString(uint32_t error_code);

#endif /* ISP_OV5640_CAMERA_SD_STORAGE_H */
