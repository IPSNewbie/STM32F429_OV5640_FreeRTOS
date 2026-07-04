#ifndef ISP_OV5640_CAMERA_IMAGE_PROCESS_H
#define ISP_OV5640_CAMERA_IMAGE_PROCESS_H

#include <stdint.h>

typedef enum
{
    CAMERA_PROCESS_OK = 0,
    CAMERA_PROCESS_ERROR = 1,
    CAMERA_PROCESS_ERROR_NULL = 2,
    CAMERA_PROCESS_ERROR_SIZE = 3,
    CAMERA_PROCESS_ERROR_MODE = 4,
} CameraImageProcessStatus_t;

typedef enum
{
    CAMERA_PROCESS_MODE_BYPASS = 0,
    CAMERA_PROCESS_MODE_GRAYSCALE = 1,
    CAMERA_PROCESS_MODE_BINARY = 2,
} CameraProcessMode_t;

typedef struct
{
    uint32_t pixel_count;
    uint32_t gray_sum;
    uint8_t gray_min;
    uint8_t gray_max;
    uint32_t shadow_count;
    uint32_t highlight_count;
    uint8_t threshold;
} CameraImageStats_t;

CameraImageProcessStatus_t Camera_ImageProcess_ApplyToFrameBuffer(CameraProcessMode_t mode,
                                                                   uint8_t binary_threshold);

CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToGrayscaleRgb565(const uint8_t *src,
                                                                               uint8_t *dst,
                                                                               uint16_t width,
                                                                               uint16_t height,
                                                                               CameraImageStats_t *stats);

CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToBinaryRgb565(const uint8_t *src,
                                                                            uint8_t *dst,
                                                                            uint16_t width,
                                                                            uint16_t height,
                                                                            uint8_t threshold,
                                                                            CameraImageStats_t *stats);

CameraImageProcessStatus_t Camera_ImageProcess_CopyRgb565(const uint8_t *src,
                                                           uint8_t *dst,
                                                           uint32_t len);

const CameraImageStats_t *Camera_ImageProcess_GetLastStats(void);

void Camera_ImageProcess_ClearLastStats(void);

#endif /* ISP_OV5640_CAMERA_IMAGE_PROCESS_H */
