#ifndef ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H
#define ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H

#include <stdint.h>  // 提供状态、计数器和返回码的固定宽度类型

/**
 * @file camera_snapshot_control.h
 * @brief SD takeover 期间的 camera 暂停状态和软件请求 guard
 *
 * 本模块只建立软件前置条件并调用 HAL_DCMI_Stop。GPIO、OV5640 0x3018、SDIO 和
 * 最终 DCMI 恢复由 camera_sd_storage 负责；guard 不是 FreeRTOS mutex。
 */

/** @name SNAPSHOT 相机控制状态
 * @{ */
#define CAMERA_SNAPSHOT_STATE_IDLE                 0U
#define CAMERA_SNAPSHOT_STATE_PREPARE_DEFERRED     1U /**< 历史保留状态，当前执行路径不写入 */
#define CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED        2U
#define CAMERA_SNAPSHOT_STATE_RESTORE_DEFERRED     3U
#define CAMERA_SNAPSHOT_STATE_ERROR                4U
/** @} */

/** @name SNAPSHOT 相机控制返回码
 * @{ */
#define CAMERA_SNAPSHOT_OK                              0U
#define CAMERA_SNAPSHOT_ERR_NOT_IMPLEMENTED             1U /**< 历史文本兼容码，当前路径不返回 */
#define CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED 2U /**< 历史码，现已真实调用 HAL_DCMI_Stop */
#define CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED 3U
#define CAMERA_SNAPSHOT_ERR_INVALID_ARGUMENT            4U
#define CAMERA_SNAPSHOT_ERR_INVALID_STATE               5U
#define CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED             6U
/** @} */

/**
 * @brief SNAPSHOT 相机停止、保护与恢复流程状态
 *
 * 保存软件 guard、DCMI 停止结果及被保护机制阻止的请求计数。
 * GPIO、0x3018、SDIO 和图像链路恢复由 SD 存储流程按固定 cleanup 顺序完成。
 */
typedef struct
{
    uint32_t prepare_attempt_count; /**< SNAPSHOT PREPARE 调用次数 */
    uint32_t restore_attempt_count; /**< SNAPSHOT RESTORE 调用次数 */
    uint32_t prepare_success_count; /**< 成功停止 DCMI 的次数 */
    uint32_t restore_success_count; /**< 历史保留的本模块硬件恢复成功次数，当前保持为 0 */
    uint32_t control_error_count;   /**< 真实相机控制错误累计次数 */
    uint32_t last_error_code;       /**< 最近一次 PREPARE 或 RESTORE 返回码 */
    uint32_t last_operation_ms;     /**< 最近一次控制调用耗时，单位 ms */
    uint32_t real_dcmi_stop_enabled;    /**< 历史策略诊断字段；当前固定为 1，不参与分支 */
    uint32_t dcmi_stop_attempt_count;   /**< HAL_DCMI_Stop 调用次数 */
    uint32_t dcmi_stop_success_count;   /**< HAL_DCMI_Stop 成功次数 */
    uint32_t dcmi_stop_error_count;     /**< HAL_DCMI_Stop 失败次数 */
    uint32_t last_dcmi_stop_hal_status; /**< 最近一次 HAL_DCMI_Stop 返回值 */

    uint32_t camera_control_state;          /**< 当前相机控制状态 */
    uint32_t dcmi_stop_required;            /**< 历史策略诊断字段，当前控制判断不读取 */
    uint32_t dcmi_dma_stop_required;        /**< 历史策略诊断字段，当前控制判断不读取 */
    uint32_t conflict_pin_release_required; /**< 历史策略诊断字段，当前控制判断不读取 */
    uint32_t camera_restore_required;       /**< 历史策略诊断字段，实际恢复由 SD cleanup 决定 */
    uint32_t frame_buffer_required;         /**< 历史策略诊断字段，prepare 由 SD 模块组织 */
    uint32_t frame_buffer_ready;            /**< 历史诊断字段，当前保持 0 且不参与放行判断 */
    uint32_t software_guard_active;         /**< SNAPSHOT 软件 guard 是否激活 */
    uint32_t dump_block_required;           /**< takeover 前置条件之一；实际 DUMP 放行读取 guard */
    uint32_t dump_block_count;              /**< 被 guard 阻止的文本 DUMP 次数 */
    uint32_t binary_block_count;            /**< 被 guard 阻止的二进制请求次数 */
} CameraSnapshotControlStatus_t;

/**
 * @brief 初始化 SNAPSHOT 控制状态
 * @note 不访问 DCMI、DMA、GPIO、SDIO 或 FatFs。
 */
void Camera_SnapshotControl_InitState(void);

/**
 * @brief 复制当前 SNAPSHOT 控制状态
 * @param status 接收状态的输出指针；空指针时直接返回
 */
void Camera_SnapshotControl_GetStatus(CameraSnapshotControlStatus_t *status);

/**
 * @brief 激活软件 guard 并停止 DCMI
 * @return CAMERA_SNAPSHOT_OK-成功，CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED-停止失败
 * @note 在 staging 完成后、GPIO takeover 前调用；失败时 guard 仍保持激活等待 cleanup。
 */
uint32_t Camera_SnapshotControl_RequestPrepare(void);

/**
 * @brief 解除软件 guard 并记录恢复请求
 * @return CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED，保留历史 deferred 语义
 * @note 本接口本身不操作硬件；GPIO、0x3018、DCMI/DMA 和连续采集由 SD 存储模块在统一 cleanup 中恢复。
 */
uint32_t Camera_SnapshotControl_RequestRestore(void);

/** @brief 查询 DUMP 是否允许执行。 @return 1-允许，0-被软件 guard 阻止 */
uint32_t Camera_SnapshotControl_IsDumpAllowed(void);

/** @brief 查询 SNAPSHOT 软件 guard。 @return 1-已激活，0-未激活 */
uint32_t Camera_SnapshotControl_IsSoftwareGuardActive(void);

/** @brief 查询相机是否已为 SNAPSHOT 暂停。 @return 1-已暂停，0-未暂停 */
uint32_t Camera_SnapshotControl_IsCameraPausedForSnapshot(void);

/**
 * @brief 查询 SDIO takeover 前置条件
 * @return 1-camera paused、guard active、dump_block_required 同时满足；否则为 0
 */
uint32_t Camera_SnapshotControl_IsTakeoverPreconditionReady(void);

/** @brief 记录一次被软件 guard 阻止的文本 DUMP。 */
void Camera_SnapshotControl_RecordDumpBlocked(void);

/** @brief 记录一次被软件 guard 阻止的二进制图像请求。 */
void Camera_SnapshotControl_RecordBinaryBlocked(void);

/**
 * @brief 将 SNAPSHOT 控制返回码转换为可读文本
 * @param error_code 控制返回码
 * @return 指向静态常量文本的指针
 */
const char *Camera_SnapshotControl_ErrorToString(uint32_t error_code);

/**
 * @brief 将 SNAPSHOT 控制状态转换为可读文本
 * @param state 控制状态码
 * @return 指向静态常量文本的指针
 */
const char *Camera_SnapshotControl_StateToString(uint32_t state);

#endif /* ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H */
