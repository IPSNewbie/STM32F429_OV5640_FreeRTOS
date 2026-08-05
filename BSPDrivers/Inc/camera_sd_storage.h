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
#define CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED 6U
#define CAMERA_SD_ERR_TAKEOVER_NOT_ACTIVE    7U
#define CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE 8U
#define CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED     9U
#define CAMERA_SD_ERR_TAKEOVER_PRECHECK_FAILED 10U
#define CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED 11U
#define CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED 12U
#define CAMERA_SD_ERR_CONFLICT_PIN_NOT_RELEASED   13U
#define CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED     14U
#define CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED    15U
#define CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED  16U
#define CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED 17U

/* SDIO 接管状态。Stage 11B-2 只会进入请求延后状态，不会进入 ACTIVE。 */
#define CAMERA_SD_TAKEOVER_STATE_IDLE             0U
#define CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED   1U
#define CAMERA_SD_TAKEOVER_STATE_ACTIVE           2U
#define CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED    3U
#define CAMERA_SD_TAKEOVER_STATE_ERROR            4U

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
    uint32_t takeover_state;     /* 当前 SDIO 接管状态。 */
    uint32_t takeover_enter_attempt_count; /* 请求进入接管模式的次数。 */
    uint32_t takeover_exit_attempt_count;  /* 请求退出接管模式的次数。 */
    uint32_t takeover_enter_success_count; /* 成功进入接管模式的次数，本阶段保持为 0。 */
    uint32_t takeover_exit_success_count;  /* 成功退出接管模式的次数，本阶段保持为 0。 */
    uint32_t takeover_error_count;         /* 接管硬件错误次数，延后请求不计为硬件错误。 */
    uint32_t last_takeover_error_code;     /* 最近一次接管请求的返回码。 */
    uint32_t last_takeover_operation_ms;   /* 最近一次接管命令的软件处理耗时。 */
    uint32_t takeover_precheck_required;      /* ENTER 前是否必须检查 SNAPSHOT 状态，固定为 1。 */
    uint32_t takeover_precheck_attempt_count; /* ENTER 前置检查次数。 */
    uint32_t takeover_precheck_success_count; /* 前置检查成功次数。 */
    uint32_t takeover_precheck_fail_count;    /* 前置检查失败次数。 */
    uint32_t snapshot_pause_required;         /* 是否要求相机处于暂停状态，固定为 1。 */
    uint32_t snapshot_pause_confirmed;        /* 最近一次前置检查是否确认相机已暂停。 */
    uint32_t conflict_pin_release_ready;      /* 软件条件是否允许进入冲突引脚释放流程。 */
    uint32_t last_takeover_precheck_error_code; /* 最近一次前置检查错误码。 */
    uint32_t conflict_pin_release_attempt_count; /* 尝试释放 PC8、PC9、PC11 的次数。 */
    uint32_t conflict_pin_release_success_count; /* 冲突引脚释放成功次数。 */
    uint32_t conflict_pin_release_error_count;   /* 冲突引脚释放失败次数。 */
    uint32_t conflict_pin_restore_attempt_count; /* 尝试恢复 PC8、PC9、PC11 的次数。 */
    uint32_t conflict_pin_restore_success_count; /* 冲突引脚恢复成功次数。 */
    uint32_t conflict_pin_restore_error_count;   /* 冲突引脚恢复失败次数。 */
    uint32_t conflict_pins_released;             /* 三个冲突引脚当前是否处于释放状态。 */
    uint32_t last_conflict_pin_error_code;       /* 最近一次冲突引脚操作错误码。 */
    uint32_t last_conflict_pin_operation_ms;     /* 最近一次冲突引脚操作耗时。 */
    uint32_t sdio_af12_switch_attempt_count;     /* 尝试切换 PC8、PC9、PC11 到 SDIO AF12 的次数。 */
    uint32_t sdio_af12_switch_success_count;     /* SDIO AF12 切换成功次数。 */
    uint32_t sdio_af12_switch_error_count;       /* SDIO AF12 切换失败次数。 */
    uint32_t sdio_af12_restore_attempt_count;    /* 尝试从 SDIO AF12 退回输入态的次数。 */
    uint32_t sdio_af12_restore_success_count;    /* 从 SDIO AF12 退回输入态成功次数。 */
    uint32_t sdio_af12_restore_error_count;      /* 从 SDIO AF12 退回输入态失败次数。 */
    uint32_t sdio_af12_selected;                 /* 三个冲突引脚当前是否处于 SDIO AF12。 */
    uint32_t last_sdio_af12_error_code;          /* 最近一次 SDIO AF12 切换或退出错误码。 */
    uint32_t last_sdio_af12_operation_ms;        /* 最近一次 SDIO AF12 切换或退出耗时。 */
    uint32_t sdio_full_gpio_switch_attempt_count; /* 尝试切换完整 SDIO GPIO 到 AF12 的次数。 */
    uint32_t sdio_full_gpio_switch_success_count; /* 完整 SDIO GPIO AF12 切换成功次数。 */
    uint32_t sdio_full_gpio_switch_error_count;   /* 完整 SDIO GPIO AF12 切换失败次数。 */
    uint32_t sdio_full_gpio_restore_attempt_count; /* 尝试将完整 SDIO GPIO 退回输入态的次数。 */
    uint32_t sdio_full_gpio_restore_success_count; /* 完整 SDIO GPIO 退回输入态成功次数。 */
    uint32_t sdio_full_gpio_restore_error_count;   /* 完整 SDIO GPIO 退回输入态失败次数。 */
    uint32_t sdio_full_gpio_af12_selected;         /* 六个 SDIO 引脚是否均处于 AF12。 */
    uint32_t last_sdio_full_gpio_error_code;       /* 最近一次完整 SDIO GPIO 操作错误码。 */
    uint32_t last_sdio_full_gpio_operation_ms;     /* 最近一次完整 SDIO GPIO 操作耗时。 */
} CameraSdStorageStatus_t;

/* 初始化纯软件状态，不访问 SDIO、GPIO 或文件系统。 */
void Camera_SDStorage_InitState(void);

/* 将当前软件状态复制到调用者提供的结构体。 */
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status);

/* 记录一次初始化请求；当前返回 NEED_TAKEOVER，不执行真实初始化。 */
uint32_t Camera_SDStorage_RequestInit(void);

/* 前置检查通过后释放冲突引脚，并将完整 SDIO GPIO 切换到 AF12。 */
uint32_t Camera_SDStorage_RequestTakeoverEnter(void);

/* 先将完整 SDIO GPIO 退回输入态，再将冲突引脚恢复为 DCMI AF13。 */
uint32_t Camera_SDStorage_RequestTakeoverExit(void);

/* 将 SD 卡模块返回码转换为 CLI 可读文本。 */
const char *Camera_SDStorage_ErrorToString(uint32_t error_code);

/* 将 SDIO 接管状态转换为 CLI 可读文本。 */
const char *Camera_SDStorage_TakeoverStateToString(uint32_t state);

#endif /* ISP_OV5640_CAMERA_SD_STORAGE_H */
