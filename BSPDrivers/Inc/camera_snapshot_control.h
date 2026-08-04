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

    uint32_t camera_control_state;          /* 当前相机控制边界状态。 */
    uint32_t dcmi_stop_required;            /* 是否必须停止 DCMI，固定为 1。 */
    uint32_t dcmi_dma_stop_required;        /* 是否必须停止 DCMI DMA，固定为 1。 */
    uint32_t conflict_pin_release_required; /* 是否必须释放 PC8、PC9、PC11，固定为 1。 */
    uint32_t camera_restore_required;       /* 保存结束后是否必须恢复相机，固定为 1。 */
    uint32_t frame_buffer_required;         /* 保存前是否必须具有稳定帧缓冲区，固定为 1。 */
    uint32_t frame_buffer_ready;            /* 帧缓冲区是否已确认可保存，本阶段固定为 0。 */
} CameraSnapshotControlStatus_t;

/* 初始化纯软件状态，不访问 DCMI、DMA、GPIO、SDIO 或 FATFS。 */
void Camera_SnapshotControl_InitState(void);

/* 将当前软件状态复制给调用者；空指针输入直接返回。 */
void Camera_SnapshotControl_GetStatus(CameraSnapshotControlStatus_t *status);

/* 记录拍照保存前的准备请求，本阶段不停止任何硬件。 */
uint32_t Camera_SnapshotControl_RequestPrepare(void);

/* 记录拍照保存后的恢复请求，本阶段不恢复任何硬件。 */
uint32_t Camera_SnapshotControl_RequestRestore(void);

/* 将相机控制边界返回码转换为 CLI 可读文本。 */
const char *Camera_SnapshotControl_ErrorToString(uint32_t error_code);

/* 将相机控制状态转换为 CLI 可读文本。 */
const char *Camera_SnapshotControl_StateToString(uint32_t state);

#endif /* ISP_OV5640_CAMERA_SNAPSHOT_CONTROL_H */
