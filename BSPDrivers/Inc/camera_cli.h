#ifndef ISP_OV5640_CAMERA_CLI_H
#define ISP_OV5640_CAMERA_CLI_H

#include "camera_image_process.h"                                // 引入图像处理模块，主要使用其中定义的 CameraProcessMode_t 处理模式类型
#include "stm32f4xx_hal.h"                                       // 引入 STM32 HAL 类型定义，主要使用 UART_HandleTypeDef 串口句柄类型
#include <stdint.h>                                              // 引入标准整数类型定义，使代码可以使用 uint8_t 等固定宽度整数类型

//============================================================================
// @file    camera_cli.h
// @brief   摄像头命令行接口（CLI）模块
// @note    CLI 是 Command Line Interface 的缩写，即“命令行接口”。
//          PC 通过 UART 发送一行文本命令，STM32 解析命令并修改运行参数。
//          该头文件只声明类型和函数，具体命令解析代码位于 camera_cli.c。
//============================================================================

//============================================================================
// 枚举类型：CLI 操作状态码
//============================================================================

typedef enum                                                    // 定义 CLI 函数执行结果枚举，用于告诉调用者命令是否处理成功
{
    CAMERA_CLI_OK = 0,                                          // 操作成功：命令格式正确，并且命令已经被正常执行

    CAMERA_CLI_ERROR = 1,                                       // 一般错误：发生了无法进一步细分的普通错误，通常作为兜底错误码

    CAMERA_CLI_ERROR_NULL = 2,                                  // 空指针错误：传入的 UART 句柄或命令字符串指针为 NULL

    CAMERA_CLI_ERROR_UNKNOWN_CMD = 3,                           // 未知命令：收到的字符串不是 CLI 当前支持的任何命令

    CAMERA_CLI_ERROR_BAD_ARG = 4,                               // 参数错误：命令名称正确，但参数缺失、格式错误或者数值超出允许范围
} CameraCliStatus_t;                                            // 枚举类型名称，CLI 处理函数通过该类型返回具体执行结果

//============================================================================
// 结构体：CLI 运行时配置
//============================================================================

typedef struct                                                   // 定义 CLI 当前运行参数结构体，用于统一保存可在线修改的图像处理配置
{
    CameraProcessMode_t process_mode;                            // 当前图像处理模式，可选旁路、灰度或二值化，具体枚举定义在 camera_image_process.h

    uint8_t binary_threshold;                                   // 二值化阈值，范围为 0～255；只有当前模式为二值化时，该参数才会影响图像结果
} CameraCliRuntimeConfig_t;                                     // 结构体类型名称，表示一份完整的 CLI 运行时配置

//============================================================================
// CLI 接口函数
//============================================================================

void Camera_CLI_Init(void);                                     // 初始化 CLI 模块：建立默认配置，将处理模式设为旁路模式，并将二值化阈值设为 128

CameraCliStatus_t Camera_CLI_HandleLine(                         // 处理一条完整的文本命令，并返回命令执行状态
    UART_HandleTypeDef *huart,                                  // UART 句柄：CLI 通过该串口向 PC 输出帮助、状态、成功或错误信息
    const char *line);                                          // 命令字符串：必须以 '\0' 结尾，并且通常已经由上层代码去掉 '\r' 和 '\n'

CameraProcessMode_t Camera_CLI_GetProcessMode(void);             // 获取当前图像处理模式，图像处理流程会调用该函数决定执行旁路、灰度还是二值化

uint8_t Camera_CLI_GetBinaryThreshold(void);                     // 获取当前二值化阈值，二值化处理时会用该值判断像素输出黑色还是白色

const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void);      // 返回内部配置结构体的只读指针，调用者可以查看配置，但不能通过该指针直接修改配置

void Camera_CLI_ResetDefault(void);                              // 恢复默认配置：处理模式恢复为旁路模式，二值化阈值恢复为 128

#endif /* ISP_OV5640_CAMERA_CLI_H */                             // 结束头文件保护条件，与文件开头的 #ifndef 相对应