#ifndef ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H
#define ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H

#include <stdint.h>

/* 拍照保存前后相机控制边界的状态。 */
#define CAMERA_SNAPSHOT_STATE_IDLE                 0U
#define CAMERA_SNAPSHOT_STATE_PREPARE_DEFERRED     1U
#define CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED        2U
#define CAMERA_SNAPSHOT_STATE_RESTORE_DEFERRED     3U
#define CAMERA_SNAPSHOT_STATE_ERROR                4U

/* 相机控制边界返回码。deferred 表示硬件操作尚未实现。 */
#define CAMERA_SNAPSHOT_OK                              0U
#define CAMERA_SNAPSHOT_ERR_NOT_IMPLEMENTED             1U
#define CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED 2U
#define CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED 3U
#define CAMERA_SNAPSHOT_ERR_INVALID_ARGUMENT            4U
#define CAMERA_SNAPSHOT_ERR_INVALID_STATE               5U
#define CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED             6U

/* 相机停止、冲突引脚释放和恢复流程的纯软件状态。 */
typedef struct
{
    uint32_t prepare_attempt_count; /* SNAPSHOT PREPARE 调用次数。 */
    uint32_t restore_attempt_count; /* SNAPSHOT RESTORE 调用次数。 */
    uint32_t prepare_success_count; /* 成功停止相机的次数，本阶段保持为 0。 */
    uint32_t restore_success_count; /* 成功恢复相机的次数，本阶段保持为 0。 */
    uint32_t control_error_count;   /* 真实控制错误次数，deferred 请求不计为错误。 */
    uint32_t last_error_code;       /* 最近一次 PREPARE 或 RESTORE 的返回码。 */
    uint32_t last_operation_ms;     /* 最近一次命令从函数入口到出口的处理耗时。 */
    uint32_t real_dcmi_stop_enabled;    /* 是否启用真实 HAL_DCMI_Stop 验证，固定为 1。 */
    uint32_t dcmi_stop_attempt_count;   /* HAL_DCMI_Stop 调用次数。 */
    uint32_t dcmi_stop_success_count;   /* HAL_DCMI_Stop 返回 HAL_OK 的次数。 */
    uint32_t dcmi_stop_error_count;     /* HAL_DCMI_Stop 返回非 HAL_OK 的次数。 */
    uint32_t last_dcmi_stop_hal_status; /* 最近一次 HAL_DCMI_Stop 返回值。 */

    uint32_t camera_control_state;          /* 当前相机控制边界状态。 */
    uint32_t dcmi_stop_required;            /* 是否必须停止 DCMI，固定为 1。 */
    uint32_t dcmi_dma_stop_required;        /* 是否必须停止 DCMI DMA，固定为 1。 */
    uint32_t conflict_pin_release_required; /* 是否必须释放 PC8、PC9、PC11，固定为 1。 */
    uint32_t camera_restore_required;       /* 保存结束后是否必须恢复相机，固定为 1。 */
    uint32_t frame_buffer_required;         /* 保存前是否必须具有稳定帧缓冲区，固定为 1。 */
    uint32_t frame_buffer_ready;            /* 帧缓冲区是否已确认可保存，本阶段固定为 0。 */
    uint32_t software_guard_active;         /* SNAPSHOT 软件保护是否已激活。 */
    uint32_t dump_block_required;           /* 当前是否需要阻止图像请求导出。 */
    uint32_t dump_block_count;              /* 软件保护阻止文本 DUMP 的次数。 */
    uint32_t binary_block_count;            /* 软件保护阻止二进制图像请求的次数。 */
} CameraSnapshotControlStatus_t;

/* 初始化 SNAPSHOT 控制状态，不访问 DCMI、DMA、GPIO、SDIO 或 FATFS。 */
void Camera_SnapshotControl_InitState(void);

/* 将当前软件状态复制给调用者；空指针输入直接返回。 */
void Camera_SnapshotControl_GetStatus(CameraSnapshotControlStatus_t *status);

/* 激活软件保护并尝试通过 HAL_DCMI_Stop 停止 DCMI。 */
uint32_t Camera_SnapshotControl_RequestPrepare(void);

/* 记录拍照保存后的恢复请求，本阶段不恢复任何硬件。 */
uint32_t Camera_SnapshotControl_RequestRestore(void);

/* 软件保护未激活时允许 DUMP，激活时禁止 DUMP。 */
uint32_t Camera_SnapshotControl_IsDumpAllowed(void);

/* 返回当前 SNAPSHOT 软件保护状态。 */
uint32_t Camera_SnapshotControl_IsSoftwareGuardActive(void);

/* 记录一次被软件保护阻止的文本 DUMP。 */
void Camera_SnapshotControl_RecordDumpBlocked(void);

/* 记录一次被软件保护阻止的二进制图像请求。 */
void Camera_SnapshotControl_RecordBinaryBlocked(void);

/* 将相机控制边界返回码转换为 CLI 可读文本。 */
const char *Camera_SnapshotControl_ErrorToString(uint32_t error_code);

/* 将相机控制状态转换为 CLI 可读文本。 */
const char *Camera_SnapshotControl_StateToString(uint32_t state);

#endif /* ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H */
