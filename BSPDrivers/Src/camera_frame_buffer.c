#include "camera_frame_buffer.h"  // 声明固定帧尺寸、front/back 双缓冲接口和帧描述结构

#include <string.h>               // 提供 memcpy()，用于把完整 RGB565 帧复制到 back buffer

//============================================================================
// @file    camera_frame_buffer.c
// @brief   160×120 RGB565 图像的 front/back 双缓冲管理
//
// 本模块只管理两块固定大小的静态内存及其角色索引，不负责启动 DCMI、
// 执行图像算法、通过 UART 发送数据或写入 SD 卡。
//
// back buffer：当前生产者使用的写入区，DCMI/DMA 或图像处理只能写这里。
// front buffer：最近一次完整提交的稳定帧，DUMP、SD SNAPSHOT 等消费者读这里。
// commit：一帧写完后交换两个索引，以 O(1) 成本发布新帧，不复制 38400 字节。
//
// 如果 DUMP 或 SD SNAPSHOT 直接读取 back buffer，读取期间 DMA/算法仍可能覆盖它，
// 就会得到同一帧前后部分来自不同采集时刻的“撕裂”图像。因此只有生产者确认
// 整帧写完后才能 commit，消费者也必须读取 front，而不能绕过本模块读取 back。
// DMA 活动期间由 CaptureTask 独占 back；完成后 ControlTask 串行组织两次 commit、处理、发送和暂存。
// 索引交换本身不提供面向任意多任务并发调用的锁或引用计数保护。
//============================================================================

// 两块 160×120×2 = 38400 字节的 RGB565 帧存放在静态区，避免占用任务栈。
// 4 字节对齐满足 DCMI DMA 按 32 位 word 搬运两个 RGB565 像素的地址要求。
static uint8_t s_camera_frame_buffers[CAMERA_FB_COUNT][CAMERA_FB_SIZE_BYTES] __attribute__((aligned(4)));

// front 索引指向消费者可稳定读取的完整帧；只有 commit 才会改变其角色。
static uint8_t s_camera_fb_front_index = 0U;

// back 索引指向生产者当前可写的帧；初始化后与 front 始终指向不同数组元素。
static uint8_t s_camera_fb_back_index = 1U;

// 恢复 front=0、back=1 的初始角色关系，供摄像头任务启动阶段调用。
// 本函数只重置索引，不清空两块大缓冲区；在首帧 commit 前其中的数据不代表新采集帧。
void Camera_FrameBuffer_Init(void)
{
    s_camera_fb_front_index = 0U;
    s_camera_fb_back_index = 1U;
}

// 返回当前 back buffer，供 DCMI/DMA 或图像处理写入下一帧。
// 调用者必须等整帧写入完成后再 commit，写入期间不能把该地址交给 DUMP 或 SD 读取。
uint8_t *Camera_FrameBuffer_GetBackBuffer(void)
{
    return s_camera_frame_buffers[s_camera_fb_back_index];
}

// 返回当前 front buffer，供 UART、SD 或其他消费者读取最近一次完整提交的帧。
// 返回的是内部存储地址而不是副本；当前架构依靠 ControlTask 串行化来避免读取中途再次 commit。
uint8_t *Camera_FrameBuffer_GetFrontBuffer(void)
{
    return s_camera_frame_buffers[s_camera_fb_front_index];
}

// 返回固定单帧字节数，供 DMA 长度、协议 payload 和复制长度采用同一尺寸来源。
uint32_t Camera_FrameBuffer_GetSizeBytes(void)
{
    return CAMERA_FB_SIZE_BYTES;
}

// 返回当前固定图像宽度；本模块不维护运行期可变分辨率。
uint16_t Camera_FrameBuffer_GetWidth(void)
{
    return (uint16_t)CAMERA_FB_WIDTH;
}

// 返回当前固定图像高度；与宽度、像素字节数共同决定 38400 字节帧长。
uint16_t Camera_FrameBuffer_GetHeight(void)
{
    return (uint16_t)CAMERA_FB_HEIGHT;
}

// 发布已经完整写入的 back buffer：交换角色后，本次 back 成为可读 front，
// 原 front 则成为下一轮可写 back。这里只交换两个 1 字节索引，不搬运图像数据。
// 必须由确认 DMA/算法已经停止写入的生产者调用，否则消费者仍可能看到撕裂帧。
CameraFrameBufferStatus_t Camera_FrameBuffer_CommitBackBuffer(void)
{
    // 先保存旧 front，避免第一次赋值后丢失下一轮 back 应使用的索引。
    uint8_t old_front_index = s_camera_fb_front_index;

    s_camera_fb_front_index = s_camera_fb_back_index;
    s_camera_fb_back_index = old_front_index;

    return CAMERA_FB_OK;
}

// 将调用者提供的一整帧 RGB565 数据复制到 back buffer，但不自动发布。
// “精确等于固定帧长”的检查可避免短拷贝留下旧像素，也可避免长拷贝越过数组边界；
// 复制成功后仍需由调用者在合适的任务/硬件时序点调用 CommitBackBuffer()。
CameraFrameBufferStatus_t Camera_FrameBuffer_CopyToBackBuffer(const uint8_t *src, uint32_t len)
{
    // 空源指针没有可复制的数据，必须在进入 memcpy() 前拒绝。
    if (src == NULL)
    {
        return CAMERA_FB_ERROR_NULL;
    }

    // 本模块只接受完整固定尺寸帧，不支持局部行或可变长度更新。
    if (len != CAMERA_FB_SIZE_BYTES)
    {
        return CAMERA_FB_ERROR_SIZE;
    }

    (void)memcpy(Camera_FrameBuffer_GetBackBuffer(), src, CAMERA_FB_SIZE_BYTES);

    return CAMERA_FB_OK;
}

// 将当前 front buffer 的地址及固定元数据填入调用者提供的 CameraFrame_t。
// 该结构只是对内部缓冲区的轻量视图，不复制 38400 字节；DUMP 可直接发送它，
// SD SNAPSHOT 则应在暂停摄像头/接管共享引脚前尽快复制到自己的 staging buffer。
CameraFrameBufferStatus_t Camera_FrameBuffer_GetFrontFrame(CameraFrame_t *frame)
{
    // 输出结构为空时无法返回帧视图，且不能继续解引用写入字段。
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
