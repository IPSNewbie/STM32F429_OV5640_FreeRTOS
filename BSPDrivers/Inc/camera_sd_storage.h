#ifndef ISP_OV5640_CAMERA_SD_STORAGE_H
#define ISP_OV5640_CAMERA_SD_STORAGE_H

#include <stdint.h>

/* SD 卡模块返回码。Stage 11C-4 允许受控初始化并读取 HAL 层卡信息。 */
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
#define CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED           18U
#define CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED         19U
#define CAMERA_SD_ERR_CARD_INFO_FAILED                20U
#define CAMERA_SD_ERR_BLOCK_READ_NOT_READY            21U
#define CAMERA_SD_ERR_BLOCK_READ_FAILED               22U
#define CAMERA_SD_ERR_BUS_WIDTH_NOT_READY              23U
#define CAMERA_SD_ERR_BUS_WIDTH_INVALID                24U
#define CAMERA_SD_ERR_BUS_WIDTH_WAIT_TRANSFER_FAILED   25U
#define CAMERA_SD_ERR_BUS_WIDTH_CONFIG_FAILED          26U

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
    uint32_t init_success_count; /* SD 卡 HAL 初始化成功次数。 */
    uint32_t init_error_count;   /* SD 卡 HAL 初始化失败次数，不计入延后请求。 */
    uint32_t last_error_code;    /* 最近一次 SD INIT 请求的返回码。 */
    uint32_t is_initialized;     /* SD 卡 HAL 初始化是否成功。 */
    uint32_t takeover_required;  /* 是否需要停止 DCMI 并由 SDIO 接管冲突引脚。 */
    uint32_t sdio_ready;         /* SDIO 是否已完成 HAL 初始化。 */
    uint32_t fatfs_ready;        /* FATFS 是否已挂载，Stage 11C-4 固定为 0。 */
    uint32_t last_operation_ms;  /* 最近一次 SD INIT 请求的处理耗时。 */
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
    uint32_t real_hal_sd_init_enabled;              /* 真实 HAL_SD_Init 路径是否启用，Stage 11C-3 固定为 1。 */
    uint32_t sdio_clock_enabled;                    /* SDIO 外设时钟当前是否已打开。 */
    uint32_t sdio_hal_init_attempt_count;           /* 实际调用 HAL_SD_Init 的次数。 */
    uint32_t sdio_hal_init_success_count;           /* HAL_SD_Init 返回 HAL_OK 的次数。 */
    uint32_t sdio_hal_init_error_count;             /* HAL_SD_Init 返回非 HAL_OK 的次数。 */
    uint32_t sdio_hal_deinit_attempt_count;         /* 实际调用 HAL_SD_DeInit 的次数。 */
    uint32_t sdio_hal_deinit_success_count;         /* HAL_SD_DeInit 返回 HAL_OK 的次数。 */
    uint32_t sdio_hal_deinit_error_count;           /* HAL_SD_DeInit 返回非 HAL_OK 的次数。 */
    uint32_t last_hal_sd_init_status;               /* 最近一次 HAL_SD_Init 返回值。 */
    uint32_t last_hal_sd_deinit_status;             /* 最近一次 HAL_SD_DeInit 返回值。 */
    uint32_t last_hal_sd_error;                     /* 最近一次 HAL_SD_GetError 返回值。 */
    uint32_t last_sdio_hal_init_operation_ms;       /* 最近一次 HAL_SD_Init 调用耗时。 */
    uint32_t last_sdio_hal_deinit_operation_ms;     /* 最近一次 HAL_SD_DeInit 调用耗时。 */
    uint32_t card_info_read_attempt_count;          /* 实际调用 HAL_SD_GetCardInfo 的次数。 */
    uint32_t card_info_read_success_count;          /* HAL_SD_GetCardInfo 返回 HAL_OK 的次数。 */
    uint32_t card_info_read_error_count;            /* HAL_SD_GetCardInfo 返回非 HAL_OK 的次数。 */
    uint32_t last_card_info_status;                 /* 最近一次 HAL_SD_GetCardInfo 返回值。 */
    uint32_t last_card_info_error;                  /* 读取卡信息后的 HAL_SD_GetError 返回值。 */
    uint32_t last_card_info_operation_ms;           /* 最近一次 HAL_SD_GetCardInfo 调用耗时。 */
    uint32_t last_hal_sd_state;                     /* 最近一次 HAL_SD_GetState 返回值。 */
    uint32_t last_hal_sd_card_state;                /* 最近一次 HAL_SD_GetCardState 返回值。 */
    uint32_t card_type;                             /* 最近一次成功读取的 CardType。 */
    uint32_t card_version;                          /* 最近一次成功读取的 CardVersion。 */
    uint32_t card_class;                            /* 最近一次成功读取的 Class。 */
    uint32_t card_rel_card_add;                     /* 最近一次成功读取的 RelCardAdd。 */
    uint32_t card_block_nbr;                        /* 最近一次成功读取的 BlockNbr。 */
    uint32_t card_block_size;                       /* 最近一次成功读取的 BlockSize。 */
    uint32_t card_log_block_nbr;                    /* 最近一次成功读取的 LogBlockNbr。 */
    uint32_t card_log_block_size;                   /* 最近一次成功读取的 LogBlockSize。 */
    uint32_t block_read_test_enabled;                /* 是否允许执行 Stage 11C-5 只读单块验证。 */
    uint32_t block_read_attempt_count;               /* 实际调用 HAL_SD_ReadBlocks 的次数。 */
    uint32_t block_read_success_count;               /* 单块读取成功次数。 */
    uint32_t block_read_error_count;                 /* 单块读取失败次数。 */
    uint32_t last_block_read_status;                 /* 最近一次 HAL_SD_ReadBlocks 返回值。 */
    uint32_t last_block_read_error;                  /* 最近一次块读取后的 HAL SD 错误码。 */
    uint32_t last_block_read_operation_ms;           /* 最近一次块读取调用耗时。 */
    uint32_t last_block_read_addr;                   /* 最近一次实际读取的逻辑块地址。 */
    uint32_t last_block_read_count;                  /* 最近一次实际读取的块数，本阶段固定为 1。 */
    uint32_t last_block_read_size;                   /* 最近一次成功读取的数据字节数。 */
    uint32_t last_block_read_sum;                    /* 最近一次成功读取数据的逐字节和。 */
    uint32_t last_block_read_xor;                    /* 最近一次成功读取数据的逐字节异或。 */
    uint32_t last_block_read_nonzero_count;          /* 最近一次成功读取数据的非零字节数。 */
    uint8_t last_block_read_first16[16];             /* 最近一次成功读取数据的前 16 字节。 */
    uint32_t block_read_wait_transfer_attempt_count; /* 读块前等待 TRANSFER 状态的次数。 */
    uint32_t block_read_wait_transfer_success_count; /* 等待 TRANSFER 状态成功次数。 */
    uint32_t block_read_wait_transfer_error_count;   /* 等待 TRANSFER 状态超时次数。 */
    uint32_t last_block_read_pre_card_state;         /* 最近一次读块调用前的 card state。 */
    uint32_t last_block_read_post_card_state;        /* 最近一次读块调用后的 card state。 */
    uint32_t last_block_read_wait_card_state;        /* 最近一次等待结束时的 card state。 */
    uint32_t last_block_read_wait_operation_ms;      /* 最近一次等待 TRANSFER 的耗时。 */
    uint32_t last_block_read_wait_timeout_ms;        /* 最近一次等待使用的超时时间。 */
    uint32_t last_block_read_error_is_data_crc_fail; /* 最近读错误是否含 DATA_CRC_FAIL。 */
    uint32_t last_block_read_error_is_cmd_crc_fail;  /* 最近读错误是否含 CMD_CRC_FAIL。 */
    uint32_t last_block_read_error_is_cmd_rsp_timeout; /* 最近读错误是否含 CMD_RSP_TIMEOUT。 */
    uint32_t last_block_read_error_is_data_timeout;  /* 最近读错误是否含 DATA_TIMEOUT。 */
    uint32_t last_block_read_error_is_rx_overrun;    /* 最近读错误是否含 RX_OVERRUN。 */
    uint32_t last_block_read_error_is_tx_underrun;   /* 最近读错误是否含 TX_UNDERRUN。 */
} CameraSdStorageStatus_t;

/* 初始化纯软件状态，不访问 SDIO、GPIO 或文件系统。 */
void Camera_SDStorage_InitState(void);

/* 将当前软件状态复制到调用者提供的结构体。 */
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status);

/* 独立调试接口：仅在 SD INIT 成功后显式配置 1-bit 或 4-bit。 */
uint32_t Camera_SDStorage_DebugSetBusWidth(uint32_t bus_width);

/* 通过日志串口输出独立 bus width 调试状态。 */
void Camera_SDStorage_DebugPrintBusWidthStatus(void);

/* 输出最近一次 SD READTEST 前后的只读 SDIO 寄存器快照。 */
void Camera_SDStorage_DebugPrintReadRegDiag(void);

/* 只读打印 SDIO GPIO 寄存器与六根信号线的当前配置和输入电平。 */
void Camera_SDStorage_PrintLineState(void);

/* 完整 SDIO GPIO 已接管时执行最小 HAL_SD_Init，否则返回 NEED_TAKEOVER。 */
uint32_t Camera_SDStorage_RequestInit(void);

/* 只读方式读取一个 512 字节逻辑块；CLI 默认请求块 0，也可指定地址。 */
uint32_t Camera_SDStorage_RequestBlockReadTest(uint32_t block_addr);

/* 前置检查通过后释放冲突引脚，并将完整 SDIO GPIO 切换到 AF12。 */
uint32_t Camera_SDStorage_RequestTakeoverEnter(void);

/* 先反初始化并关闭 SDIO 时钟，再退出 AF12 并恢复冲突引脚的 DCMI AF13。 */
uint32_t Camera_SDStorage_RequestTakeoverExit(void);

/* 将 SD 卡模块返回码转换为 CLI 可读文本。 */
const char *Camera_SDStorage_ErrorToString(uint32_t error_code);

/* 将 SDIO 接管状态转换为 CLI 可读文本。 */
const char *Camera_SDStorage_TakeoverStateToString(uint32_t state);

#endif /* ISP_OV5640_CAMERA_SD_STORAGE_H */
