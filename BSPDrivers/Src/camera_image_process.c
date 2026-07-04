#include "camera_image_process.h"
#include "camera_frame_buffer.h"

#include <string.h>

#define CAMERA_IMAGE_PROCESS_SHADOW_THRESHOLD     30U
#define CAMERA_IMAGE_PROCESS_HIGHLIGHT_THRESHOLD  240U

static CameraImageStats_t s_camera_image_last_stats;

static uint8_t Camera_ImageProcess_Rgb565ToGray(uint16_t pixel)
{
    uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(pixel & 0x1FU);
    uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

    return (uint8_t)((77U * r8 + 150U * g8 + 29U * b8) >> 8);
}

static uint16_t Camera_ImageProcess_GrayToRgb565(uint8_t gray)
{
    return (uint16_t)((((uint16_t)(gray >> 3)) << 11) |
                      (((uint16_t)(gray >> 2)) << 5) |
                      ((uint16_t)(gray >> 3)));
}

static void Camera_ImageProcess_ResetStats(CameraImageStats_t *stats, uint8_t threshold)
{
    if (stats == NULL)
    {
        return;
    }

    stats->pixel_count = 0U;
    stats->gray_sum = 0U;
    stats->gray_min = 0xFFU;
    stats->gray_max = 0U;
    stats->shadow_count = 0U;
    stats->highlight_count = 0U;
    stats->threshold = threshold;
}

static void Camera_ImageProcess_UpdateStats(CameraImageStats_t *stats, uint8_t gray)
{
    if (stats == NULL)
    {
        return;
    }

    stats->pixel_count++;
    stats->gray_sum += gray;

    if (gray < stats->gray_min)
    {
        stats->gray_min = gray;
    }

    if (gray > stats->gray_max)
    {
        stats->gray_max = gray;
    }

    if (gray < CAMERA_IMAGE_PROCESS_SHADOW_THRESHOLD)
    {
        stats->shadow_count++;
    }

    if (gray > CAMERA_IMAGE_PROCESS_HIGHLIGHT_THRESHOLD)
    {
        stats->highlight_count++;
    }
}

static CameraImageProcessStatus_t Camera_ImageProcess_CalcRgb565Stats(const uint8_t *src,
                                                                      uint16_t width,
                                                                      uint16_t height,
                                                                      uint8_t threshold,
                                                                      CameraImageStats_t *stats)
{
    uint32_t pixel_count;

    if ((src == NULL) || (stats == NULL))
    {
        return CAMERA_PROCESS_ERROR_NULL;
    }

    if ((width != CAMERA_FB_WIDTH) || (height != CAMERA_FB_HEIGHT))
    {
        return CAMERA_PROCESS_ERROR_SIZE;
    }

    pixel_count = (uint32_t)width * (uint32_t)height;
    Camera_ImageProcess_ResetStats(stats, threshold);

    for (uint32_t i = 0U; i < pixel_count; ++i)
    {
        uint16_t pixel = (uint16_t)(((uint16_t)src[(2U * i) + 1U] << 8) |
                                    ((uint16_t)src[2U * i]));
        uint8_t gray = Camera_ImageProcess_Rgb565ToGray(pixel);

        Camera_ImageProcess_UpdateStats(stats, gray);
    }

    return CAMERA_PROCESS_OK;
}

CameraImageProcessStatus_t Camera_ImageProcess_CopyRgb565(const uint8_t *src,
                                                           uint8_t *dst,
                                                           uint32_t len)
{
    if ((src == NULL) || (dst == NULL))
    {
        return CAMERA_PROCESS_ERROR_NULL;
    }

    if (len != CAMERA_FB_SIZE_BYTES)
    {
        return CAMERA_PROCESS_ERROR_SIZE;
    }

    (void)memcpy(dst, src, len);
    return CAMERA_PROCESS_OK;
}

CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToGrayscaleRgb565(const uint8_t *src,
                                                                               uint8_t *dst,
                                                                               uint16_t width,
                                                                               uint16_t height,
                                                                               CameraImageStats_t *stats)
{
    uint32_t pixel_count;

    if ((src == NULL) || (dst == NULL) || (stats == NULL))
    {
        return CAMERA_PROCESS_ERROR_NULL;
    }

    if ((width != CAMERA_FB_WIDTH) || (height != CAMERA_FB_HEIGHT))
    {
        return CAMERA_PROCESS_ERROR_SIZE;
    }

    pixel_count = (uint32_t)width * (uint32_t)height;
    Camera_ImageProcess_ResetStats(stats, 0U);

    for (uint32_t i = 0U; i < pixel_count; ++i)
    {
        uint16_t pixel = (uint16_t)(((uint16_t)src[(2U * i) + 1U] << 8) |
                                    ((uint16_t)src[2U * i]));
        uint8_t gray = Camera_ImageProcess_Rgb565ToGray(pixel);
        uint16_t gray565 = Camera_ImageProcess_GrayToRgb565(gray);

        dst[2U * i] = (uint8_t)(gray565 & 0xFFU);
        dst[(2U * i) + 1U] = (uint8_t)(gray565 >> 8);

        Camera_ImageProcess_UpdateStats(stats, gray);
    }

    return CAMERA_PROCESS_OK;
}

CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToBinaryRgb565(const uint8_t *src,
                                                                            uint8_t *dst,
                                                                            uint16_t width,
                                                                            uint16_t height,
                                                                            uint8_t threshold,
                                                                            CameraImageStats_t *stats)
{
    uint32_t pixel_count;

    if ((src == NULL) || (dst == NULL) || (stats == NULL))
    {
        return CAMERA_PROCESS_ERROR_NULL;
    }

    if ((width != CAMERA_FB_WIDTH) || (height != CAMERA_FB_HEIGHT))
    {
        return CAMERA_PROCESS_ERROR_SIZE;
    }

    pixel_count = (uint32_t)width * (uint32_t)height;
    Camera_ImageProcess_ResetStats(stats, threshold);

    for (uint32_t i = 0U; i < pixel_count; ++i)
    {
        uint16_t pixel = (uint16_t)(((uint16_t)src[(2U * i) + 1U] << 8) |
                                    ((uint16_t)src[2U * i]));
        uint8_t gray = Camera_ImageProcess_Rgb565ToGray(pixel);
        uint16_t binary565 = (gray >= threshold) ? 0xFFFFU : 0x0000U;

        dst[2U * i] = (uint8_t)(binary565 & 0xFFU);
        dst[(2U * i) + 1U] = (uint8_t)(binary565 >> 8);

        Camera_ImageProcess_UpdateStats(stats, gray);
    }

    return CAMERA_PROCESS_OK;
}

CameraImageProcessStatus_t Camera_ImageProcess_ApplyToFrameBuffer(CameraProcessMode_t mode,
                                                                   uint8_t binary_threshold)
{
    CameraFrame_t front_frame;
    uint8_t *back_buffer;
    CameraImageProcessStatus_t process_ret;

    if (Camera_FrameBuffer_GetFrontFrame(&front_frame) != CAMERA_FB_OK)
    {
        return CAMERA_PROCESS_ERROR;
    }

    back_buffer = Camera_FrameBuffer_GetBackBuffer();
    if ((front_frame.data == NULL) || (back_buffer == NULL))
    {
        return CAMERA_PROCESS_ERROR_NULL;
    }

    if ((front_frame.width != CAMERA_FB_WIDTH) ||
        (front_frame.height != CAMERA_FB_HEIGHT) ||
        (front_frame.size_bytes != CAMERA_FB_SIZE_BYTES))
    {
        return CAMERA_PROCESS_ERROR_SIZE;
    }

    if (mode == CAMERA_PROCESS_MODE_BYPASS)
    {
        process_ret = Camera_ImageProcess_CopyRgb565(front_frame.data,
                                                     back_buffer,
                                                     front_frame.size_bytes);
        if (process_ret == CAMERA_PROCESS_OK)
        {
            process_ret = Camera_ImageProcess_CalcRgb565Stats(front_frame.data,
                                                              front_frame.width,
                                                              front_frame.height,
                                                              binary_threshold,
                                                              &s_camera_image_last_stats);
        }
    }
    else if (mode == CAMERA_PROCESS_MODE_GRAYSCALE)
    {
        process_ret = Camera_ImageProcess_ConvertRgb565ToGrayscaleRgb565(front_frame.data,
                                                                          back_buffer,
                                                                          front_frame.width,
                                                                          front_frame.height,
                                                                          &s_camera_image_last_stats);
    }
    else if (mode == CAMERA_PROCESS_MODE_BINARY)
    {
        process_ret = Camera_ImageProcess_ConvertRgb565ToBinaryRgb565(front_frame.data,
                                                                       back_buffer,
                                                                       front_frame.width,
                                                                       front_frame.height,
                                                                       binary_threshold,
                                                                       &s_camera_image_last_stats);
    }
    else
    {
        return CAMERA_PROCESS_ERROR_MODE;
    }

    if (process_ret != CAMERA_PROCESS_OK)
    {
        return process_ret;
    }

    if (Camera_FrameBuffer_CommitBackBuffer() != CAMERA_FB_OK)
    {
        return CAMERA_PROCESS_ERROR;
    }

    return CAMERA_PROCESS_OK;
}

const CameraImageStats_t *Camera_ImageProcess_GetLastStats(void)
{
    return &s_camera_image_last_stats;
}

void Camera_ImageProcess_ClearLastStats(void)
{
    Camera_ImageProcess_ResetStats(&s_camera_image_last_stats, 0U);
}
