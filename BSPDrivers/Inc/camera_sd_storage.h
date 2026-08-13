#ifndef ISP_OV5640_CAMERA_SD_STORAGE_H
#define ISP_OV5640_CAMERA_SD_STORAGE_H

#include <stdbool.h>
#include <stdint.h>  // 提供错误码、状态字段、扇区号和耗时的固定宽度类型

/**
 * @file camera_sd_storage.h
 * @brief SDIO 共享引脚接管、FatFs 与 BMP24 快照保存接口
 *
 * 快照先通过 Camera RTOS 公共路径准备 RGB565 front，并复制到独立 staging；随后暂停
 * camera、清 OV5640 0x3018[6:4]、把 PC8/PC9/PC11 从 DCMI AF13 切到 SDIO AF12，
 * 按 1-bit polling 访问 FatFs。所有路径最终按文件→文件系统→SDIO/GPIO→DVP/DCMI
 * 顺序清理。SD STATUS 只读取缓存，与显式 mount/snapshot 操作严格分离。
 */

#ifndef CAMERA_SD_INIT_CLOCK_DIV
/** @brief SDIO 初始化阶段使用的时钟分频值。 */
#define CAMERA_SD_INIT_CLOCK_DIV (1U)
#endif

/** @name SD 存储结果码
 * @note 返回码覆盖共享引脚接管、SDIO polling、FatFs、BMP 写入和相机恢复阶段。
 * @{ */
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
#define CAMERA_SD_ERR_SNAPSHOT_BUSY                31U
#define CAMERA_SD_ERR_SNAPSHOT_PREPARE_FAILED      32U
#define CAMERA_SD_ERR_FATFS_MOUNT_FAILED           33U
#define CAMERA_SD_ERR_FATFS_UNMOUNT_FAILED         34U
#define CAMERA_SD_ERR_FATFS_DISK_NOT_READY         35U
#define CAMERA_SD_ERR_FATFS_DISK_READ_FAILED       36U
#define CAMERA_SD_ERR_FATFS_DISK_IOCTL_FAILED      37U
#define CAMERA_SD_ERR_FATFS_CARD_TIMEOUT           38U
#define CAMERA_SD_ERR_INVALID_ARGUMENT             39U
#define CAMERA_SD_ERR_FATFS_DISK_WRITE_FAILED      40U
#define CAMERA_SD_ERR_FATFS_FILE_OPEN_FAILED       41U
#define CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED      42U
#define CAMERA_SD_ERR_FATFS_FILE_CLOSE_FAILED      43U
#define CAMERA_SD_ERR_SDIO_CLOCK_DISABLE_FAILED    44U
#define CAMERA_SD_ERR_CAMERA_RESTORE_FAILED        45U
#define CAMERA_SD_ERR_FRAME_BUFFER_INVALID         46U
#define CAMERA_SD_ERR_FRAME_EMPTY                  47U
#define CAMERA_SD_ERR_FRAME_PREPARE_FAILED         48U
#define CAMERA_SD_ERR_FRAME_PREPARE_TIMEOUT        49U
#define CAMERA_SD_ERR_FILE_SCAN_FAILED             50U
#define CAMERA_SD_ERR_FILE_INDEX_FULL              51U
/** @} */

/**
 * @brief 单次 SD SNAPSHOT 的执行结果
 *
 * 保存图像来源、格式、文件名、各阶段耗时和首个错误信息，供 CLI 输出本次操作结果。
 */
typedef struct
{
    const char *file_name;   /**< 本次分配的 IMGxxxx.BMP 文件名 */
    uint32_t bytes_written;  /**< 实际写入 BMP 文件的字节数 */
    const char *source_text; /**< 图像来源状态文本 */
    uint32_t source_bytes;   /**< staging buffer 中的 RGB565 字节数 */
    uint32_t source_nonzero; /**< staging buffer 非零字节数量 */
    uint32_t source_sum32;   /**< staging buffer 字节和的低 32 位 */
    const char *prepare_text; /**< 图像准备阶段状态文本 */
    uint32_t prepare_retry;   /**< 图像准备重试次数 */
    const char *format_text;  /**< 保存格式文本，当前为 BMP24 */
    uint32_t width;           /**< 保存图像宽度，单位像素 */
    uint32_t height;          /**< 保存图像高度，单位像素 */
    const char *mount_text;   /**< FatFs 挂载结果文本 */
    const char *write_text;   /**< BMP 写入结果文本 */
    const char *cleanup_text; /**< 文件、挂载和 SDIO 清理结果文本 */
    const char *restore_text; /**< 相机链路恢复结果文本 */
    uint32_t total_ms;        /**< 本次 SD SNAPSHOT 总耗时，单位 ms */
    uint32_t prepare_ms;      /**< 图像准备与 staging 复制耗时，单位 ms */
    uint32_t write_ms;        /**< 文件写入耗时，单位 ms */
    uint32_t cleanup_ms;      /**< cleanup 与硬件恢复耗时，单位 ms */
    uint32_t error_code;      /**< 本次操作首个错误码 */
    const char *error_text;   /**< error_code 对应的稳定文本 */
} CameraSdSnapshotResult_t;

/**
 * @brief SD 存储运行状态缓存
 *
 * 保存最近一次挂载、BMP 写入、错误码、耗时及 DVP/SDIO 诊断信息。
 * SD STATUS 只复制该缓存，不触发 SDIO、FatFs 或摄像头硬件访问。
 */
typedef struct
{
    uint32_t supported;       /**< 当前固件是否支持 SD 存储 */
    uint32_t card_ready;      /**< 最近一次会话缓存；不表示查询瞬间卡仍处于初始化状态 */
    uint32_t takeover_required; /**< 是否需要接管 DCMI/SDIO 共享引脚 */
    uint32_t sdio_ready;      /**< 最近一次会话中 SDIO 是否初始化成功 */
    uint32_t fatfs_ready;     /**< 最近一次会话中 FatFs 是否挂载成功 */
    uint32_t last_mount_result; /**< 最近一次 FatFs 挂载结果码 */
    const char *last_mount_text; /**< 最近一次挂载结果文本 */
    const char *last_snapshot_text; /**< 最近一次 SNAPSHOT 结果文本 */
    const char *last_file_name; /**< 指向模块静态文件名缓存，不需要调用方释放 */
    uint32_t last_file_size;  /**< 最近一次成功保存的文件大小，单位字节 */
    uint32_t save_count;      /**< 成功保存图片的累计次数 */
    uint32_t save_error_code; /**< 最近一次保存错误码 */
    const char *save_error_text; /**< 最近一次保存错误文本 */
    uint32_t last_error_code; /**< 最近一次 SD 存储操作错误码 */
    const char *last_error_text; /**< 最近一次操作错误文本 */
    uint32_t last_total_ms;   /**< 最近一次 SD SNAPSHOT 总耗时，单位 ms */
    uint32_t last_write_ms;   /**< 最近一次 BMP 写入耗时，单位 ms */
    uint32_t dvp_mask_available; /**< OV5640 0x3018 DVP mask 是否可用 */
    uint32_t dvp_mask_active; /**< D2/D3/D4 输出当前是否被临时屏蔽 */
    uint32_t dvp_reg_3018_saved; /**< takeover 前保存的 0x3018 值 */
    uint32_t dvp_reg_3018_current_or_restored; /**< 最近读回或恢复后的 0x3018 值 */
    uint32_t last_sd_init_status; /**< 最近一次 HAL SD 初始化状态 */
    uint32_t last_sd_init_error; /**< 最近一次 HAL SD 初始化错误码 */
    uint32_t last_sd_rw_status;  /**< 最近一次 HAL SD 读写状态 */
    uint32_t last_sd_rw_error;   /**< 最近一次 HAL SD 读写错误码 */
} CameraSdStorageStatus_t;

/**
 * @brief 初始化 SD 存储状态缓存
 * @note 只清除软件状态，不访问 SD 卡、FatFs 或共享引脚。
 */
void Camera_SDStorage_InitState(void);

/** Create the depth-one Storage request/result queues. */
bool Camera_SDStorage_TaskInit(void);

/** StorageTask entry; waits indefinitely for an already-staged snapshot. */
void Camera_SDStorage_Task(void *argument);

/** StorageTask historical minimum remaining stack, in bytes. */
uint32_t Camera_SDStorage_GetStackMinFreeBytes(void);

/**
 * @brief 获取 SD 存储缓存状态
 * @param status 接收状态副本的输出指针；空指针时直接返回
 * @note 该接口是 SD STATUS 的数据源，必须保持纯只读，不触发 SDIO 或 FatFs 操作。
 */
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status);

/**
 * @brief 执行一次只读 FatFs 挂载检查并完整清理
 * @return CAMERA_SD_OK-成功，其他值-准备、接管、初始化、挂载或恢复失败
 * @note 这是显式硬件探测，会执行 takeover 和 mount；与纯缓存 SD STATUS 不同。
 *       无论中途是否失败，均进入统一 cleanup 恢复相机硬件状态。
 */
uint32_t Camera_SDStorage_CheckFatfsMount(void);

/**
 * @brief 将当前 RGB565 帧保存为递增命名的 BMP24 文件
 * @param snapshot_result 接收文件信息、耗时和错误状态的输出结构；允许为空
 * @return CAMERA_SD_OK-成功，其他值-图像准备、SD/FatFs、写入或恢复失败
 * @note 先通过统一 RTOS 接口准备图像并复制到 staging buffer，再接管共享引脚；
 *       这样写卡期间不依赖已暂停的摄像头 front buffer。BMP 按行转换为 BGR888，
 *       避免额外分配 57600 字节的 BMP 全帧缓冲区。
 */
uint32_t Camera_SDStorage_SaveSnapshotFrame(
    CameraSdSnapshotResult_t *snapshot_result);

/**
 * @brief 查询活动 FatFs takeover 会话内部的磁盘状态
 * @return CAMERA_SD_OK-就绪，其他值-未就绪
 * @note 该 diskio 接口可能访问 HAL 卡状态，不能与纯缓存的 CLI SD STATUS 混淆。
 */
uint32_t Camera_SDStorage_FatFsDiskStatus(void);

/**
 * @brief 验证既有 FatFs SD 会话并等待卡进入 TRANSFER
 * @return SD 存储结果码
 * @note 名称来自 FatFs diskio ABI；不会重新执行 HAL_SD_Init。
 */
uint32_t Camera_SDStorage_FatFsDiskInitialize(void);

/**
 * @brief 通过 HAL SD polling 接口读取连续扇区
 * @param buffer 接收扇区数据的缓冲区
 * @param sector 起始逻辑扇区号
 * @param count 连续扇区数量，必须大于 0 且不能超出卡容量
 * @return SD 存储结果码；参数或扇区范围非法时返回 CAMERA_SD_ERR_INVALID_ARGUMENT
 * @note 仅供活动 takeover 会话中的 diskio 回调使用，采用 HAL SD polling 且有 timeout。
 */
uint32_t Camera_SDStorage_FatFsDiskRead(
    uint8_t *buffer,
    uint32_t sector,
    uint32_t count);

/**
 * @brief 通过 HAL SD polling 接口写入连续扇区
 * @param buffer 待写入扇区数据
 * @param sector 起始逻辑扇区号
 * @param count 连续扇区数量，必须大于 0 且不能超出卡容量
 * @return SD 存储结果码；参数或扇区范围非法时返回 CAMERA_SD_ERR_INVALID_ARGUMENT
 * @note 仅在显式开启写允许的 FatFs takeover 会话中执行，采用 polling，不启用 DMA/IRQ。
 */
uint32_t Camera_SDStorage_FatFsDiskWrite(
    const uint8_t *buffer,
    uint32_t sector,
    uint32_t count);

/**
 * @brief 处理 FatFs 同步和介质参数查询命令
 * @param command FatFs 磁盘控制命令
 * @param buffer 命令输出缓冲区；需要输出的命令不可为空
 * @return SD 存储结果码
 */
uint32_t Camera_SDStorage_FatFsDiskIoctl(uint8_t command, void *buffer);

/**
 * @brief 临时关闭 OV5640 D2/D3/D4 输出
 * @return SD 存储结果码
 * @note 保存原值后执行 saved_3018 & 0x8F；0x8F=1000 1111b，清零 bit[6:4]。
 *       这些位控制 D2/D3/D4，分别复用 PC8/PC9/PC11 与 SDIO D0/D1/D3。
 */
uint32_t Camera_SDStorage_StopDvpConflictPads(void);

/**
 * @brief 恢复 takeover 前保存的 OV5640 0x3018 DVP 输出配置
 * @return SD 存储结果码
 * @note 必须在 GPIO 已退出 SDIO AF12 后恢复，使传感器重新驱动 DVP 数据线。
 */
uint32_t Camera_SDStorage_RestoreDvpConflictPads(void);

/**
 * @brief 将共享 GPIO 切换为 SDIO AF12
 * @return SD 存储结果码
 * @note 调用前必须已暂停 camera、激活 guard 并屏蔽 0x3018[6:4]；该操作把共享
 *       GPIO 的所有权从 DCMI AF13 临时转给 SDIO AF12。
 */
uint32_t Camera_SDStorage_RequestTakeoverEnter(void);

/**
 * @brief 反初始化 SD、关闭时钟并把共享 GPIO 恢复到安全状态
 * @return SD 存储结果码
 * @note cleanup 顺序用于确保 SDIO 停止驱动总线后，才恢复 DVP 和相机链路。
 */
uint32_t Camera_SDStorage_RequestTakeoverExit(void);

/**
 * @brief 以现有 SDIO 1-bit polling 策略初始化 SD 卡
 * @return SD 存储结果码
 * @note 保持硬件验证过的 1-bit polling 策略，不启用 SDIO DMA 或 IRQ。
 */
uint32_t Camera_SDStorage_RequestInit(void);

/**
 * @brief 将 SD 存储错误码转换为稳定文本
 * @param error_code SD 存储错误码
 * @return 指向静态常量文本的指针
 */
const char *Camera_SDStorage_ErrorToString(uint32_t error_code);

#endif /* ISP_OV5640_CAMERA_SD_STORAGE_H */
