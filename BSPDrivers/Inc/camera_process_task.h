#ifndef ISP_OV5640_CAMERA_PROCESS_TASK_H
#define ISP_OV5640_CAMERA_PROCESS_TASK_H

#include <stdint.h>

#include "camera_image_process.h"

/** Future Stage 15D ProcessTask request result. */
typedef enum
{
    CAMERA_PROCESS_RESULT_OK = 0U,
    CAMERA_PROCESS_RESULT_NOT_IMPLEMENTED = 1U
} CameraProcessResult_t;

/**
 * Compile-only Stage 15D API skeleton. Stage 15D-D0 has no runtime caller.
 */
CameraProcessResult_t Camera_ProcessRequestFrame(CameraProcessMode_t mode,
                                                 uint8_t threshold,
                                                 uint32_t timeout_ms);

#endif /* ISP_OV5640_CAMERA_PROCESS_TASK_H */
