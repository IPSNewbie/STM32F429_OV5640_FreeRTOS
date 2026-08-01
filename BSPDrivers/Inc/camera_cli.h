#ifndef ISP_OV5640_CAMERA_CLI_H                         // 头文件保护宏：如果该宏还没有被定义，说明这是本文件第一次被包含
#define ISP_OV5640_CAMERA_CLI_H                         // 定义头文件保护宏，防止 camera_cli.h 被重复包含后出现类型或函数重复声明

#include "camera_image_process.h"                       // 引入图像处理模块，主要使用其中定义的 CameraProcessMode_t 图像处理模式类型
#include "stm32f4xx_hal.h"                              // 引入 STM32 HAL 库类型定义，主要使用 UART_HandleTypeDef 串口句柄类型
#include <stdint.h>                                     // 引入标准固定宽度整数类型，使代码可以使用 uint8_t、uint16_t、uint32_t 等类型

// ============================================================================
// camera_cli 模块作用：
// 1. 接收已经解析完成的一整行文本命令，例如 "HELP"、"STATUS"、"PROC GRAY"。
// 2. 根据命令内容查询或修改摄像头运行参数。
// 3. 通过指定 UART 向 PC 返回命令执行结果。
// 4. 当前模块主要管理图像处理模式和二值化阈值。
// 5. 这里只声明数据类型和公开函数，具体命令判断与执行过程位于 camera_cli.c。
// ============================================================================

// ============================================================================
// CLI 函数返回状态
// ============================================================================

typedef enum                                             // 定义 CLI 操作结果枚举，调用者可根据返回值判断命令是否执行成功
{
    CAMERA_CLI_OK = 0,                                   // 命令执行成功：命令名称、参数格式和参数范围都正确，相关操作已经完成

    CAMERA_CLI_ERROR = 1,                                // 通用错误：发生了未被其他错误码具体描述的错误，通常作为兜底错误状态

    CAMERA_CLI_ERROR_NULL = 2,                           // 空指针错误：传入的 UART 句柄、命令字符串或其他必要指针为 NULL

    CAMERA_CLI_ERROR_UNKNOWN_CMD = 3,                    // 未知命令错误：收到的命令不属于 HELP、STATUS、PROC、THR、RESET 等已支持命令

    CAMERA_CLI_ERROR_BAD_ARG = 4,                        // 参数错误：命令名称正确，但参数缺失、拼写错误、格式错误或数值超出允许范围
} CameraCliStatus_t;                                     // CameraCliStatus_t 表示 Camera_CLI_HandleLine() 等函数的执行结果类型

// ============================================================================
// CLI 运行时配置
// ============================================================================

typedef struct                                           // 定义 CLI 当前运行参数结构体，用于集中保存能够通过串口命令在线修改的配置
{
    CameraProcessMode_t process_mode;                    // 当前图像处理模式，可为 BYPASS、GRAYSCALE 或 BINARY，枚举定义在 camera_image_process.h

    uint8_t binary_threshold;                            // 当前二值化阈值，范围为 0～255；灰度值大于等于该阈值时通常输出白色，否则输出黑色
} CameraCliRuntimeConfig_t;                              // CameraCliRuntimeConfig_t 表示 CLI 模块当前保存的全部运行时配置

// ============================================================================
// CLI 对外接口
// ============================================================================

void Camera_CLI_Init(void);                              // 初始化 CLI 模块，并把运行参数恢复为默认值：处理模式为 BYPASS，二值化阈值为 128

CameraCliStatus_t Camera_CLI_HandleLine(                 // 处理一条已经接收完整的文本命令，并根据命令内容执行查询、设置或复位操作
    UART_HandleTypeDef *huart,                           // huart 指向用于返回文本响应的 UART 句柄；当前项目通常传入 USART1 对应的 &huart1
    const char *line);                                   // line 指向以 '\0' 结尾的命令字符串；字符串中应已经去除 '\r' 和 '\n' 换行字符

CameraProcessMode_t Camera_CLI_GetProcessMode(void);     // 返回当前图像处理模式，图像处理流程会根据该值选择 BYPASS、GRAYSCALE 或 BINARY

uint8_t Camera_CLI_GetBinaryThreshold(void);             // 返回当前二值化阈值，只有图像处理模式为 BINARY 时该阈值才会直接影响输出图像

const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(    // 返回 CLI 内部运行时配置结构体的只读指针，供 STATUS 等功能一次读取全部配置
    void);                                               // 函数没有输入参数；返回值带 const，调用者只能读取配置，不能通过该指针修改内部变量

void Camera_CLI_ResetDefault(void);                      // 将 CLI 配置恢复为默认值：process_mode 恢复为 BYPASS，binary_threshold 恢复为 128

#endif /* ISP_OV5640_CAMERA_CLI_H */                     // 结束头文件保护范围，防止同一编译单元中重复包含本头文件