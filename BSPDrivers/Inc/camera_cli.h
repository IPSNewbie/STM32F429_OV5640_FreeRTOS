#ifndef ISP_OV5640_CAMERA_CLI_H
#define ISP_OV5640_CAMERA_CLI_H

#include "camera_image_process.h"  // BYPASS/GRAY/BINARY 模式和图像统计类型
#include "stm32f4xx_hal.h"         // CLI 文本响应使用的 UART 句柄

#include <stdint.h>                 // 提供二值化阈值的固定宽度整数类型

/**
 * @file camera_cli.h
 * @brief CameraServiceTask 使用的文本 CLI 和运行配置接口
 *
 * 上游先形成 NUL 结尾完整文本行，再调用 HandleLine()。PROC/THR/RESET 只更新配置，
 * 下一次帧准备才读取；STATUS 与 SD STATUS 只读缓存；SD SNAPSHOT 同步执行存储流程。
 * DUMP 在上游 camera_pc_dump 中转换为业务事件，不由 HandleLine() 直接执行。
 */

/**
 * @brief UART CLI 命令处理结果
 */
typedef enum
{
    CAMERA_CLI_OK = 0,                /**< 命令处理成功 */
    CAMERA_CLI_ERROR = 1,             /**< 当前用于 SD SNAPSHOT 总流程失败 */
    CAMERA_CLI_ERROR_NULL = 2,        /**< UART 句柄或命令行为空 */
    CAMERA_CLI_ERROR_UNKNOWN_CMD = 3, /**< 未识别的命令 */
    CAMERA_CLI_ERROR_BAD_ARG = 4      /**< 命令参数非法 */
} CameraCliStatus_t;

/**
 * @brief CLI 可修改、帧准备流程读取的运行期配置缓存
 * @note 这些值不是 OV5640 硬件寄存器；修改后不会立即启动采集或改写当前 front。
 */
typedef struct
{
    CameraProcessMode_t process_mode; /**< 当前 BYPASS、GRAY 或 BINARY 模式 */
    uint8_t binary_threshold;         /**< 二值化阈值，范围 0~255 */
} CameraCliRuntimeConfig_t;

/**
 * @brief 初始化 CLI、SD 状态缓存和 snapshot guard，并恢复默认图像配置
 * @note 启动阶段调用；本函数不初始化 SDIO、不 mount FatFs，也不执行快照。
 */
void Camera_CLI_Init(void);

/**
 * @brief 处理一行完整的 UART CLI 命令
 * @param huart 用于发送命令响应的 UART 句柄
 * @param line 以空字符结尾的命令行
 * @return 命令处理结果，见 @ref CameraCliStatus_t
 * @note 在 CameraServiceTask 中调用；输入必须是 NUL 结尾完整行，正常上游最多 31 字符。
 *       HELP 固定包含 8 项；DUMP 已由上游处理，SD SNAPSHOT 会在本调用中同步执行。
 */
CameraCliStatus_t Camera_CLI_HandleLine(
    UART_HandleTypeDef *huart,
    const char *line);

/**
 * @brief 获取当前图像处理模式
 * @return 当前 BYPASS、GRAY 或 BINARY 模式
 * @note 由同一 CameraServiceTask 的帧准备流程读取，无额外锁保护。
 */
CameraProcessMode_t Camera_CLI_GetProcessMode(void);

/**
 * @brief 获取当前二值化阈值
 * @return 阈值，范围 0~255
 * @note BINARY 模式以 gray >= threshold 作为白色分界。
 */
uint8_t Camera_CLI_GetBinaryThreshold(void);

/**
 * @brief 获取 CLI 运行配置的只读指针
 * @return 指向内部静态配置的只读指针
 * @note 指针生命周期覆盖整个程序，但内容会被后续 CLI 命令更新，不是不可变副本。
 */
const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void);

/**
 * @brief 恢复 BYPASS 模式和 128 阈值
 * @note 只修改配置缓存，不立即处理图像或启动 DCMI。
 */
void Camera_CLI_ResetDefault(void);

#endif /* ISP_OV5640_CAMERA_CLI_H */
