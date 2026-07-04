#ifndef ISP_OV5640_CAMERA_FRAME_BUFFER_H
#define ISP_OV5640_CAMERA_FRAME_BUFFER_H

#include <stdint.h>

#define CAMERA_FB_WIDTH              160U
#define CAMERA_FB_HEIGHT             120U
#define CAMERA_FB_BYTES_PER_PIXEL    2U
#define CAMERA_FB_PIXEL_COUNT        (CAMERA_FB_WIDTH * CAMERA_FB_HEIGHT)
#define CAMERA_FB_SIZE_BYTES         (CAMERA_FB_PIXEL_COUNT * CAMERA_FB_BYTES_PER_PIXEL)
#define CAMERA_FB_COUNT              2U

typedef enum
{
    CAMERA_FB_OK = 0,
    CAMERA_FB_ERROR = 1,
    CAMERA_FB_ERROR_NULL = 2,
    CAMERA_FB_ERROR_SIZE = 3,
} CameraFrameBufferStatus_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint32_t size_bytes;
    uint8_t *data;
} CameraFrame_t;

void Camera_FrameBuffer_Init(void);

uint8_t *Camera_FrameBuffer_GetBackBuffer(void);
uint8_t *Camera_FrameBuffer_GetFrontBuffer(void);

uint32_t Camera_FrameBuffer_GetSizeBytes(void);
uint16_t Camera_FrameBuffer_GetWidth(void);
uint16_t Camera_FrameBuffer_GetHeight(void);

CameraFrameBufferStatus_t Camera_FrameBuffer_CommitBackBuffer(void);

CameraFrameBufferStatus_t Camera_FrameBuffer_CopyToBackBuffer(const uint8_t *src, uint32_t len);
CameraFrameBufferStatus_t Camera_FrameBuffer_GetFrontFrame(CameraFrame_t *frame);

#endif /* ISP_OV5640_CAMERA_FRAME_BUFFER_H */
