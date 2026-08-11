#ifndef ISP_OV5640_CAMERA_CLI_H
#define ISP_OV5640_CAMERA_CLI_H

#include "camera_command.h"        // CLI 解析输出和执行入口使用的统一命令值对象
#include "camera_image_process.h"  // BYPASS/GRAY/BINARY 模式和图像统计类型
#include "stm32f4xx_hal.h"         // CLI 文本响应使用的 UART 句柄

#include <stdint.h>                 // 提供二值化阈值的固定宽度整数类型

/**
 * @file camera_cli.h
 * @brief CommTask parser 与 ControlTask executor 共用的文本 CLI 接口
 *
 * 上游先形成 NUL 结尾完整文本行，再调用 HandleLine()。HandleLine() 只识别命令、
 * 校验参数并向 CommandQueue 提交值对象；ControlTask 出队后调用 ExecuteCommand()
 * 执行原有文本输出、配置或 SD 业务。DUMP 仍由上游文本行模块形成命令事件。
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
    CAMERA_CLI_ERROR_BAD_ARG = 4,     /**< 命令参数非法 */
    CAMERA_CLI_ERROR_QUEUE_FULL = 5   /**< CommandQueue 未初始化或没有空闲槽位 */
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
 * @brief 解析一行完整的 UART CLI 命令并提交 CameraCommand_t
 * @param huart 当前 UART 句柄；保留参数校验兼容，parser 本身不进行 UART TX
 * @param line 以空字符结尾的命令行
 * @return 解析和提交结果，见 @ref CameraCliStatus_t
 * @note 输入必须是 NUL 结尾完整行，正常上游最多 31 字符。有效命令和既有 parser
 *       错误都会形成值对象；HELP 固定包含 8 项，DUMP 仍由上游识别后提交。
 */
CameraCliStatus_t Camera_CLI_HandleLine(
    UART_HandleTypeDef *huart,
    const char *line);

/**
 * @brief 在 ControlTask 中执行一个已经出队的文本 CLI 命令
 * @param huart 用于保持原有 CLI 文本响应的 UART 句柄
 * @param command 已从 CommandQueue 复制出的稳定命令值对象
 * @return 命令执行结果；DUMP 和 IMAGE_REQUEST 不由本接口处理
 */
CameraCliStatus_t Camera_CLI_ExecuteCommand(
    UART_HandleTypeDef *huart,
    const CameraCommand_t *command);

/**
 * @brief 获取当前图像处理模式
 * @return 当前 BYPASS、GRAY 或 BINARY 模式
 * @note 由同一 ControlTask 的帧准备流程读取，无额外锁保护。
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
