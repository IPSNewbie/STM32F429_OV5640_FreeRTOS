#include "camera_frame_buffer.h"

#include <string.h>

static uint8_t s_camera_frame_buffers[CAMERA_FB_COUNT][CAMERA_FB_SIZE_BYTES] __attribute__((aligned(4)));
static uint8_t s_camera_fb_front_index = 0U;
static uint8_t s_camera_fb_back_index = 1U;

void Camera_FrameBuffer_Init(void)
{
    s_camera_fb_front_index = 0U;
    s_camera_fb_back_index = 1U;
}

uint8_t *Camera_FrameBuffer_GetBackBuffer(void)
{
    return s_camera_frame_buffers[s_camera_fb_back_index];
}

uint8_t *Camera_FrameBuffer_GetFrontBuffer(void)
{
    return s_camera_frame_buffers[s_camera_fb_front_index];
}

uint32_t Camera_FrameBuffer_GetSizeBytes(void)
{
    return CAMERA_FB_SIZE_BYTES;
}

uint16_t Camera_FrameBuffer_GetWidth(void)
{
    return (uint16_t)CAMERA_FB_WIDTH;
}

uint16_t Camera_FrameBuffer_GetHeight(void)
{
    return (uint16_t)CAMERA_FB_HEIGHT;
}

CameraFrameBufferStatus_t Camera_FrameBuffer_CommitBackBuffer(void)
{
    uint8_t old_front_index = s_camera_fb_front_index;

    s_camera_fb_front_index = s_camera_fb_back_index;
    s_camera_fb_back_index = old_front_index;

    return CAMERA_FB_OK;
}

CameraFrameBufferStatus_t Camera_FrameBuffer_CopyToBackBuffer(const uint8_t *src, uint32_t len)
{
    if (src == NULL)
    {
        return CAMERA_FB_ERROR_NULL;
    }

    if (len != CAMERA_FB_SIZE_BYTES)
    {
        return CAMERA_FB_ERROR_SIZE;
    }

    (void)memcpy(Camera_FrameBuffer_GetBackBuffer(), src, CAMERA_FB_SIZE_BYTES);

    return CAMERA_FB_OK;
}

CameraFrameBufferStatus_t Camera_FrameBuffer_GetFrontFrame(CameraFrame_t *frame)
{
    if (frame == NULL)
    {
        return CAMERA_FB_ERROR_NULL;
    }

    frame->width = (uint16_t)CAMERA_FB_WIDTH;
    frame->height = (uint16_t)CAMERA_FB_HEIGHT;
    frame->size_bytes = CAMERA_FB_SIZE_BYTES;
    frame->data = Camera_FrameBuffer_GetFrontBuffer();

    return CAMERA_FB_OK;
}
