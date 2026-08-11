#include "camera_image_process.h"  // 处理模式、统计结构和算法公开接口
#include "camera_frame_buffer.h"   // 稳定 front、可写 back 和 commit 双缓冲接口

#include <string.h>                 // BYPASS 完整帧复制使用 memcpy

//============================================================================
// @file    camera_image_process.c
// @brief   固定 160x120 RGB565 帧的 BYPASS、GRAY、BINARY 处理模块
//
// 本模块不启动 DCMI，也不直接发送或存储图像。ControlTask 完成原始帧采集并
// 第一次 commit 后，从稳定 front 读取，在另一个 back 中生成完整结果；只有算法全部
// 成功才再次 commit，使处理结果成为 DUMP/SD SNAPSHOT 可读的新 front。
//
// BYPASS 仍复制 front→back，以保持三种模式统一的读写/发布语义；GRAY 把亮度重新
// 编码成 RGB565；BINARY 按阈值输出 0x0000/0xFFFF。所有模式同时维护最近一帧的
// 灰度、阴影和高光统计。安全性依赖 ControlTask 串行调用，不可在 ISR 或
// 另一任务并发处理同一双缓冲。
//============================================================================

// 灰度严格小于 30 才计为阴影；等于 30 不计入。
#define CAMERA_IMAGE_PROCESS_SHADOW_THRESHOLD     30U
// 灰度严格大于 240 才计为高光；等于 240 不计入。
#define CAMERA_IMAGE_PROCESS_HIGHLIGHT_THRESHOLD  240U

// 最近一次处理统计的模块缓存；返回指针长期有效，但下一帧会覆盖内容。
static CameraImageStats_t s_camera_image_last_stats;

// 按固定点亮度权重将一个 RGB565 像素转换为 8 位灰度。
// bit[15:11]=R5、bit[10:5]=G6、bit[4:0]=B5；位复制近似扩展到 8 bit。
// 77/150/29 总和为 256，近似 0.299R+0.587G+0.114B，右移 8 位避免浮点运算。
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

// 将同一 8 位灰度写入 RGB565 三通道：R/B 取高 5 位、G 取高 6 位，形成无色彩图像。
static uint16_t Camera_ImageProcess_GrayToRgb565(uint8_t gray)
{
    return (uint16_t)((((uint16_t)(gray >> 3)) << 11) |
                      (((uint16_t)(gray >> 2)) << 5) |
                      ((uint16_t)(gray >> 3)));
}

// 清空一帧统计并保存本次阈值快照；min=255/max=0 让首个样本能更新两个极值。
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

// 累计一个原始灰度样本；gray_sum/pixel_count 可计算平均值。
// 固定 19200 像素的最大总和可放入 uint32_t，BINARY 统计的仍是阈值前灰度。
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

// 固定扫描 160x120=19200 个 RGB565 像素并计算统计。
// 每个像素按低字节 src[2*i]、高字节 src[2*i+1] 组合，最后访问 38398/38399；
// 循环次数固定，不等待硬件且无需 timeout。threshold 这里只记录，不执行二值化。
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

// 为 BYPASS 分支完整复制 38400 字节 front→back；精确长度检查防止发布残帧。
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

// 将 19200 个输入像素逐个转灰度后仍编码为两字节 RGB565，并同步累计统计。
// 循环固定 19200 次，输出低字节在前，因此帧尺寸和 UART 协议格式保持不变。
CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToGrayscaleRgb565(const uint8_t *src,
                                                                               uint8_t *dst,
                                                                               uint16_t width,
                                                                               uint16_t height,
                                                                               CameraImageStats_t *stats)
{
    uint32_t pixel_count;  // 固定尺寸下为 19200，限定下面像素循环上界

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

// 按阈值将固定帧转换为黑白 RGB565：gray>=threshold 为白，否则为黑。
// 统计仍基于二值化前的原始灰度；循环和字节访问边界与灰度模式相同。
CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToBinaryRgb565(const uint8_t *src,
                                                                            uint8_t *dst,
                                                                            uint16_t width,
                                                                            uint16_t height,
                                                                            uint8_t threshold,
                                                                            CameraImageStats_t *stats)
{
    uint32_t pixel_count;  // 固定尺寸下为 19200，限定下面像素循环上界

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

// 由 Camera_RTOS_PrepareRgb565Frame 在 ControlTask 中发布最终处理帧。
// 调用前原始 DCMI back 已第一次 commit 为稳定 front；本函数只读该 front、写另一
// back。任何取帧、尺寸、算法或 commit 失败都不发布半帧，原 front 继续保持稳定。
CameraImageProcessStatus_t Camera_ImageProcess_ApplyToFrameBuffer(CameraProcessMode_t mode,
                                                                   uint8_t binary_threshold)
{
    CameraFrame_t front_frame;  // 已提交、不会被本轮算法写入的源帧描述
    uint8_t *back_buffer;       // 与 front 不同的完整结果目标缓冲
    CameraImageProcessStatus_t process_ret;  // 决定结果是否允许 commit

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

    // BYPASS 也走 front→back→commit，保证模式切换后发布路径和统计行为一致。
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

    // 只有整帧处理成功才交换索引；commit 后结果成为 DUMP/SD 可读的稳定 front。
    if (Camera_FrameBuffer_CommitBackBuffer() != CAMERA_FB_OK)
    {
        return CAMERA_PROCESS_ERROR;
    }

    return CAMERA_PROCESS_OK;
}

// 返回内部实时统计视图；指针长期有效，但后续处理/Clear 会覆盖内容，不用于同步。
const CameraImageStats_t *Camera_ImageProcess_GetLastStats(void)
{
    return &s_camera_image_last_stats;
}

// 只恢复统计字段的初始值，不清空或交换任何图像缓冲区。
void Camera_ImageProcess_ClearLastStats(void)
{
    Camera_ImageProcess_ResetStats(&s_camera_image_last_stats, 0U);
}
