#ifndef ISP_OV5640_CAMERA_CAPTURE_H
#define ISP_OV5640_CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

/** CaptureRequestQueue stores one small request; frame data stays in the buffers. */
#define CAMERA_CAPTURE_REQUEST_QUEUE_DEPTH 1U

/** Result of one synchronous raw-frame capture request. */
typedef enum
{
    CAMERA_CAPTURE_OK = 0U,
    CAMERA_CAPTURE_START_FAILED = 1U,
    CAMERA_CAPTURE_TIMEOUT = 2U,
    CAMERA_CAPTURE_HAL_ERROR = 3U,
    CAMERA_CAPTURE_NOT_READY = 4U,
    CAMERA_CAPTURE_REQUEST_FAILED = 5U
} CameraCaptureResult_t;

/** Minimal CaptureTask health data used by MonitorTask. */
typedef struct
{
    volatile uint32_t heartbeat_count;
    volatile uint32_t stack_min_free_bytes;
    volatile uint32_t last_start_status;
    volatile CameraCaptureResult_t last_result;
} CameraCaptureStats_t;

/** Create the depth-one CaptureRequestQueue before the scheduler starts. */
bool Camera_CaptureInit(void);

/** Submit one capture request and synchronously wait for CaptureTask completion. */
CameraCaptureResult_t Camera_CaptureRequestFrame(uint32_t timeout_ms);

/** CaptureTask entry: owns DCMI/DMA and the back buffer while DMA is active. */
void Camera_CaptureTask(void *argument);

/** Notify CaptureTask of a completed frame from the DCMI ISR. */
BaseType_t Camera_CaptureNotifyFrameCompleteFromISR(void);

/** Notify CaptureTask of a HAL DCMI/DMA error from an ISR. */
BaseType_t Camera_CaptureNotifyErrorFromISR(void);

/** Return the internal read-only CaptureTask health view. */
const CameraCaptureStats_t *Camera_CaptureGetStats(void);

#endif /* ISP_OV5640_CAMERA_CAPTURE_H */
