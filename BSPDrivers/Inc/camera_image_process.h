#ifndef ISP_OV5640_CAMERA_IMAGE_PROCESS_H
#define ISP_OV5640_CAMERA_IMAGE_PROCESS_H

#include <stdint.h>  // 提供 RGB565 像素、统计计数和阈值的固定宽度类型

/**
 * @file camera_image_process.h
 * @brief 固定 160x120 RGB565 双缓冲图像处理接口
 *
 * ControlTask 从稳定 front 读取，在 back 中生成 BYPASS、GRAY 或 BINARY 结果，
 * 完整成功后才 commit。输出格式始终保持 RGB565，因此 DUMP 和 SD SNAPSHOT 无需
 * 随模式改变帧尺寸或协议。
 */

/**
 * @brief 图像处理操作结果
 */
typedef enum
{
    CAMERA_PROCESS_OK = 0,         /**< 处理成功 */
    CAMERA_PROCESS_ERROR = 1,      /**< 获取 front 或最终 commit 等双缓冲操作失败 */
    CAMERA_PROCESS_ERROR_NULL = 2, /**< 输入、输出或统计指针为空 */
    CAMERA_PROCESS_ERROR_SIZE = 3, /**< 图像尺寸或数据长度非法 */
    CAMERA_PROCESS_ERROR_MODE = 4, /**< 未支持的处理模式 */
} CameraImageProcessStatus_t;

/**
 * @brief 可选的 RGB565 图像处理模式
 */
typedef enum
{
    CAMERA_PROCESS_MODE_BYPASS = 0,    /**< 完整复制 front→back，并保留统计/commit 流程 */
    CAMERA_PROCESS_MODE_GRAYSCALE = 1, /**< 灰度结果仍重新编码为 RGB565 */
    CAMERA_PROCESS_MODE_BINARY = 2,    /**< gray>=threshold 输出白，否则输出黑的 RGB565 */
} CameraProcessMode_t;

/**
 * @brief 最近一帧图像的灰度统计
 */
typedef struct
{
    uint32_t pixel_count;     /**< 参与统计的像素数 */
    uint32_t gray_sum;        /**< 所有像素灰度值之和，可与 pixel_count 计算平均值 */
    uint8_t gray_min;         /**< 最小灰度值 */
    uint8_t gray_max;         /**< 最大灰度值 */
    uint32_t shadow_count;    /**< 灰度严格小于 30 的像素数量 */
    uint32_t highlight_count; /**< 灰度严格大于 240 的像素数量 */
    uint8_t threshold;        /**< 关联阈值：BINARY 为实际值，GRAY 为 0，BYPASS 为当前配置 */
} CameraImageStats_t;

/**
 * @brief 对当前 front frame 应用选定处理并提交处理结果
 * @param mode BYPASS、GRAY 或 BINARY 模式
 * @param binary_threshold 二值化阈值；非 BINARY 模式下保留该参数
 * @return 图像处理结果
 * @note 由 ControlTask 调用。输入来自稳定 front，输出写 back；任何失败都不
 *       commit，避免 DUMP/SD 读取半处理帧。模块本身不提供并发锁。
 */
CameraImageProcessStatus_t Camera_ImageProcess_ApplyToFrameBuffer(CameraProcessMode_t mode,
                                                                   uint8_t binary_threshold);

/**
 * @brief 将 RGB565 图像转换为 RGB565 灰度图并生成统计
 * @param src 源 RGB565 图像
 * @param dst 目标 RGB565 图像缓冲区
 * @param width 图像宽度，单位为像素
 * @param height 图像高度，单位为像素
 * @param stats 接收灰度统计的输出结构
 * @return 图像处理结果
 * @note RGB565 为 R5/G6/B5；输出仍为两字节 RGB565，尺寸必须等于固定 frame buffer。
 */
CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToGrayscaleRgb565(const uint8_t *src,
                                                                               uint8_t *dst,
                                                                               uint16_t width,
                                                                               uint16_t height,
                                                                               CameraImageStats_t *stats);

/**
 * @brief 将 RGB565 图像按阈值转换为黑白 RGB565 图并生成统计
 * @param src 源 RGB565 图像
 * @param dst 目标 RGB565 图像缓冲区
 * @param width 图像宽度，单位为像素
 * @param height 图像高度，单位为像素
 * @param threshold 二值化阈值，范围 0~255
 * @param stats 接收灰度统计的输出结构
 * @return 图像处理结果
 * @note gray 等于 threshold 时归白；统计基于阈值前灰度，而不是 0/255 输出。
 */
CameraImageProcessStatus_t Camera_ImageProcess_ConvertRgb565ToBinaryRgb565(const uint8_t *src,
                                                                            uint8_t *dst,
                                                                            uint16_t width,
                                                                            uint16_t height,
                                                                            uint8_t threshold,
                                                                            CameraImageStats_t *stats);

/**
 * @brief 复制一段 RGB565 图像数据
 * @param src 源数据
 * @param dst 目标缓冲区
 * @param len 复制长度，必须等于 CAMERA_FB_SIZE_BYTES
 * @return 图像处理结果
 * @note 当前用于 BYPASS，src/dst 是独立 front/back，len 必须是一帧 38400 字节。
 */
CameraImageProcessStatus_t Camera_ImageProcess_CopyRgb565(const uint8_t *src,
                                                           uint8_t *dst,
                                                           uint32_t len);

/**
 * @brief 获取最近一次图像处理统计
 * @return 指向内部静态统计结构的只读指针
 * @note 指针长期有效，但下一次处理或 Clear 会覆盖内容，不是跨任务同步快照。
 */
const CameraImageStats_t *Camera_ImageProcess_GetLastStats(void);

/**
 * @brief 恢复最近一次图像统计的初始值
 * @note 不修改 front/back 图像内容，也不执行 commit。
 */
void Camera_ImageProcess_ClearLastStats(void);

#endif /* ISP_OV5640_CAMERA_IMAGE_PROCESS_H */
