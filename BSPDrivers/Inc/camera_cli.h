#ifndef ISP_OV5640_CAMERA_CLI_H
#define ISP_OV5640_CAMERA_CLI_H

#include "camera_image_process.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

//============================================================================
// @file    camera_cli.h
// @brief   摄像头命令行接口（CLI）模块
// @note    提供通过 UART 解析用户命令、配置图像处理参数的功能。
//          支持设置处理模式（旁路/灰度/二值化）和二值化阈值。
//============================================================================

//============================================================================
// 枚举类型：CLI 操作状态码
//============================================================================

/**
 * @brief  CLI 操作状态码
 */
typedef enum
{
    CAMERA_CLI_OK = 0,                /**< 操作成功 */
    CAMERA_CLI_ERROR = 1,             /**< 一般错误 */
    CAMERA_CLI_ERROR_NULL = 2,        /**< 空指针错误 */
    CAMERA_CLI_ERROR_UNKNOWN_CMD = 3, /**< 未知命令 */
    CAMERA_CLI_ERROR_BAD_ARG = 4,     /**< 参数错误 */
} CameraCliStatus_t;

//============================================================================
// 结构体：CLI 运行时配置
//============================================================================

/**
 * @brief  CLI 运行时配置结构体
 * @note   保存当前通过 CLI 设置的图像处理参数
 */
typedef struct
{
    CameraProcessMode_t process_mode; /**< 当前处理模式（旁路/灰度/二值化） */
    uint8_t binary_threshold;         /**< 二值化阈值（0~255），仅二值化模式有效 */
} CameraCliRuntimeConfig_t;

//============================================================================
// CLI 接口函数
//============================================================================

/**
 * @brief  初始化 CLI 模块
 * @note   将运行时配置重置为默认值（旁路模式，阈值 128）
 */
void Camera_CLI_Init(void);

/**
 * @brief  处理一行用户输入的命令（通过 UART）
 * @param  huart  UART 句柄，用于发送响应信息
 * @param  line   以 '\0' 结尾的命令行字符串（不含换行符）
 * @retval CAMERA_CLI_OK              命令解析并执行成功
 * @retval CAMERA_CLI_ERROR_NULL      huart 或 line 为 NULL
 * @retval CAMERA_CLI_ERROR_UNKNOWN_CMD  未知命令
 * @retval CAMERA_CLI_ERROR_BAD_ARG   命令参数格式错误或超出范围
 * @note   支持的命令格式示例：
 *         - "mode bypass"    : 设为旁路模式
 *         - "mode grayscale" : 设为灰度模式
 *         - "mode binary"    : 设为二值化模式
 *         - "threshold 200"  : 设置二值化阈值为 200（0~255）
 *         - "status"         : 打印当前配置
 *         - "help"           : 打印帮助信息
 *         命令不区分大小写，参数以空格分隔。
 */
CameraCliStatus_t Camera_CLI_HandleLine(UART_HandleTypeDef *huart, const char *line);

/**
 * @brief  获取当前设置的处理模式
 * @return 当前 @ref CameraProcessMode_t 值
 */
CameraProcessMode_t Camera_CLI_GetProcessMode(void);

/**
 * @brief  获取当前设置的二值化阈值
 * @return 当前阈值（0~255）
 */
uint8_t Camera_CLI_GetBinaryThreshold(void);

/**
 * @brief  获取当前完整配置的只读指针
 * @return 指向内部静态 @ref CameraCliRuntimeConfig_t 的指针
 */
const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void);

/**
 * @brief  将配置重置为默认值（旁路模式，阈值 128）
 */
void Camera_CLI_ResetDefault(void);

#endif /* ISP_OV5640_CAMERA_CLI_H */
