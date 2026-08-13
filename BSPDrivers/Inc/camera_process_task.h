#ifndef ISP_OV5640_CAMERA_PROCESS_TASK_H
#define ISP_OV5640_CAMERA_PROCESS_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "camera_image_process.h"

#define CAMERA_PROCESS_REQUEST_QUEUE_DEPTH  1U
#define CAMERA_PROCESS_RESULT_QUEUE_DEPTH   1U

/** ProcessTask completion result returned to ControlTask. */
typedef enum
{
    CAMERA_PROCESS_RESULT_OK = CAMERA_PROCESS_OK,
    CAMERA_PROCESS_RESULT_ERROR = CAMERA_PROCESS_ERROR,
    CAMERA_PROCESS_RESULT_ERROR_NULL = CAMERA_PROCESS_ERROR_NULL,
    CAMERA_PROCESS_RESULT_ERROR_SIZE = CAMERA_PROCESS_ERROR_SIZE,
    CAMERA_PROCESS_RESULT_ERROR_MODE = CAMERA_PROCESS_ERROR_MODE,
    CAMERA_PROCESS_RESULT_NOT_READY = 5U,
    CAMERA_PROCESS_RESULT_REQUEST_FAILED = 6U,
    CAMERA_PROCESS_RESULT_TIMEOUT = 7U
} CameraProcessResult_t;

/** Small metadata snapshot sent by ControlTask; no frame payload is copied. */
typedef struct
{
    uint32_t request_id;
    CameraProcessMode_t mode;
    uint8_t threshold;
} CameraProcessRequest_t;

/** Correlated completion returned independently from Capture notifications. */
typedef struct
{
    uint32_t request_id;
    CameraProcessResult_t result;
} CameraProcessResponse_t;

/** Create the two depth-one queues used by the Stage 15D processing path. */
bool Camera_ProcessTaskInit(void);

/** Submit one synchronous request using a single bounded timeout budget. */
CameraProcessResult_t Camera_ProcessRequestFrame(CameraProcessMode_t mode,
                                                 uint8_t threshold,
                                                 uint32_t timeout_ms);

/** ProcessTask entry; blocks indefinitely on ProcessRequestQueue while idle. */
void Camera_ProcessTask(void *argument);

/** True only when no accepted request is queued or executing. */
bool Camera_ProcessTaskIsIdle(void);

/** ProcessTask historical minimum remaining stack, in bytes. */
uint32_t Camera_ProcessTaskGetStackMinFreeBytes(void);

#endif /* ISP_OV5640_CAMERA_PROCESS_TASK_H */
