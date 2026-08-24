#ifndef ISP_OV5640_CAMERA_FRAME_BUFFER_H
#define ISP_OV5640_CAMERA_FRAME_BUFFER_H

#include <stdint.h>  // 提供 uint8_t、uint16_t、uint32_t 等固定宽度帧数据类型

/**
 * @file camera_frame_buffer.h
 * @brief 160×120 RGB565 front/back 双缓冲的尺寸定义和访问接口
 *
 * DCMI/DMA 或图像处理属于帧生产者，只能写入 back buffer；UART DUMP、
 * SD SNAPSHOT 等帧消费者只能读取已经提交的 front buffer。生产者写完一整帧后
 * 调用 @ref Camera_FrameBuffer_CommitBackBuffer 交换角色，从而避免消费者读到
 * 正在变化的像素。接口返回的是内部静态缓冲区地址，不会分配动态内存。
 *
 * @note DMA 活动期间 CaptureTask 独占 back buffer；采集完成后由 ControlTask 串行执行
 *       commit、图像处理和再次 commit。该模块不创建互斥锁。
 */

/** @brief 固定帧宽度 160 像素，与 PC DUMP、图像处理和 BMP 输入尺寸保持一致。 */
#define CAMERA_FB_WIDTH              160U
/** @brief 固定帧高度 120 像素；本模块不支持运行期改变分辨率。 */
#define CAMERA_FB_HEIGHT             120U
/** @brief RGB565 每像素为 16 bit，因此占 2 字节。 */
#define CAMERA_FB_BYTES_PER_PIXEL    2U
/** @brief 单帧像素总数，即 160×120 = 19200。 */
#define CAMERA_FB_PIXEL_COUNT        (CAMERA_FB_WIDTH * CAMERA_FB_HEIGHT)
/** @brief 单帧长度，即 160×120×2 = 38400 字节。 */
#define CAMERA_FB_SIZE_BYTES         (CAMERA_FB_PIXEL_COUNT * CAMERA_FB_BYTES_PER_PIXEL)
/** @brief 两块缓冲分别承担稳定读取和下一帧写入角色，commit 后交换角色。 */
#define CAMERA_FB_COUNT              2U

/**
 * @brief 帧缓冲接口的统一返回状态
 *
 * 调用者可据此区分参数为空、帧长度不匹配和通用失败；接口不会通过异常或
 * 动态分配报告错误。
 */
typedef enum
{
    CAMERA_FB_OK = 0,         /**< 操作成功 */
    CAMERA_FB_ERROR = 1,      /**< 通用操作失败 */
    CAMERA_FB_ERROR_NULL = 2, /**< 输入或输出指针为空 */
    CAMERA_FB_ERROR_SIZE = 3, /**< 数据长度与固定帧尺寸不匹配 */
} CameraFrameBufferStatus_t;

/**
 * @brief 当前稳定 front frame 的元数据视图
 *
 * 该结构只保存尺寸、长度和内部缓冲区指针，不拥有也不复制图像数据。
 * 虽然 data 的 C 类型不是 const，DUMP 和 SD 等消费者仍应把它作为只读数据使用。
 */
typedef struct
{
    uint16_t width;      /**< front frame 宽度，当前固定为 CAMERA_FB_WIDTH */
    uint16_t height;     /**< front frame 高度，当前固定为 CAMERA_FB_HEIGHT */
    uint32_t size_bytes; /**< RGB565 payload 长度，当前为 CAMERA_FB_SIZE_BYTES */
    uint8_t *data;       /**< 内部 front buffer 起始地址；消费者只读且不负责释放 */
} CameraFrame_t;

/**
 * @brief 初始化 front/back 双缓冲及其索引
 * @return 无
 * @note 将 0 号缓冲设为 front、1 号缓冲设为 back，但不会清空图像内存。
 *       应在摄像头任务开始采集前调用；首帧写完并 commit 后 front 才代表新图像。
 */
void Camera_FrameBuffer_Init(void);

/**
 * @brief 获取供 DCMI 或图像处理写入的 back buffer
 * @return 当前 back buffer 起始地址，指向 CAMERA_FB_SIZE_BYTES 字节静态内存
 * @note 该地址可能正在被生产者写入，禁止 DUMP 或 SD SNAPSHOT 将其作为稳定帧读取。
 */
uint8_t *Camera_FrameBuffer_GetBackBuffer(void);

/**
 * @brief 获取供发送、保存或处理读取的 front buffer
 * @return 当前 front buffer 起始地址，指向最近一次完整提交的帧
 * @note 返回内部指针而非数据副本。调用期间不应由其他上下文并发 commit。
 */
uint8_t *Camera_FrameBuffer_GetFrontBuffer(void);

/**
 * @brief 获取固定单帧 RGB565 字节数
 * @return CAMERA_FB_SIZE_BYTES，即 38400 字节
 */
uint32_t Camera_FrameBuffer_GetSizeBytes(void);

/**
 * @brief 获取固定图像宽度
 * @return CAMERA_FB_WIDTH，即 160 像素
 */
uint16_t Camera_FrameBuffer_GetWidth(void);

/**
 * @brief 获取固定图像高度
 * @return CAMERA_FB_HEIGHT，即 120 像素
 */
uint16_t Camera_FrameBuffer_GetHeight(void);

/**
 * @brief 将已写完的 back buffer 提交为新的 front buffer
 * @return CAMERA_FB_OK
 * @note 该操作只交换索引，不复制 38400 字节图像数据，因此耗时与帧大小无关。
 *       必须在 DCMI/DMA 或图像算法完成整帧写入后调用。提交后旧 front 自动成为
 *       下一轮 back，DUMP 和 SD 只能读取新 front，不能直接读取 back。
 */
CameraFrameBufferStatus_t Camera_FrameBuffer_CommitBackBuffer(void);

/**
 * @brief 将一帧 RGB565 数据复制到 back buffer
 * @param src 源图像首地址，必须非 NULL，且在复制期间保持有效
 * @param len 源数据长度，必须精确等于 CAMERA_FB_SIZE_BYTES
 * @return CAMERA_FB_OK-复制成功；CAMERA_FB_ERROR_NULL-源指针为空；
 *         CAMERA_FB_ERROR_SIZE-长度不是完整固定帧
 * @note 本函数只复制到 back，不自动 commit；调用者确认整帧准备完成后再发布。
 */
CameraFrameBufferStatus_t Camera_FrameBuffer_CopyToBackBuffer(const uint8_t *src, uint32_t len);

/**
 * @brief 获取当前 front buffer 的帧视图
 * @param frame 输出结构，接收固定尺寸、payload 长度和内部 front 指针
 * @return CAMERA_FB_OK-成功，CAMERA_FB_ERROR_NULL-输出指针为空
 * @note 不复制像素，也不转移内存所有权。DUMP 可在当前串行任务中直接发送该帧；
 *       SD SNAPSHOT 会先把它复制到 staging buffer，再切换 DCMI/SDIO 共享引脚。
 */
CameraFrameBufferStatus_t Camera_FrameBuffer_GetFrontFrame(CameraFrame_t *frame);

#endif /* ISP_OV5640_CAMERA_FRAME_BUFFER_H */
