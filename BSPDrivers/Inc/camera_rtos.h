#ifndef CAMERA_RTOS_H
#define CAMERA_RTOS_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

//============================================================================
// @file    camera_rtos.h
// @brief   摄像头 RTOS 任务模块
// @note    定义基于 FreeRTOS 的两个任务：
//          - CameraServiceTask：摄像头服务任务（处理帧采集、PC Dump、CLI）
//          - MonitorTask：监控任务（定期维护心跳和运行时间）
//          同时提供运行统计信息的获取接口。
//============================================================================

//============================================================================
// 枚举：RTOS 运行错误码
//============================================================================

/**
 * @brief  RTOS 运行错误码
 * @note   基础错误码用于 STATUS 显示，分阶段错误码的低 8 位保留子错误码
 */
typedef enum
{
    CAMERA_RTOS_ERR_NONE = 0x00000000U,                /**< 无错误 */
    CAMERA_RTOS_ERR_UART_NULL = 0x00000001U,           /**< UART 句柄为空 */
    CAMERA_RTOS_ERR_DUMP_FAILED = 0x00000002U,         /**< DUMP 失败 */
    CAMERA_RTOS_ERR_BAD_STATE = 0x00000003U,           /**< 运行状态异常 */
    CAMERA_RTOS_ERR_UNKNOWN_CMD = 0x00000004U,         /**< 未知 CLI 命令 */
    CAMERA_RTOS_ERR_UART_DMA_INIT = 0x00000005U,       /**< UART DMA 初始化失败 */
    CAMERA_RTOS_ERR_UART_DMA_RECOVERY = 0x00000006U,   /**< UART DMA 需要恢复 */
    CAMERA_RTOS_ERR_STREAM_OVERFLOW = 0x00000007U,     /**< StreamBuffer 溢出 */
    CAMERA_RTOS_ERR_SNAPSHOT_START_BASE = 0x00000100U, /**< 快照启动失败基础码 */
    CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT = 0x00000200U,    /**< 快照等待超时 */
    CAMERA_RTOS_ERR_CAPTURE_COMMIT_BASE = 0x00000300U, /**< 帧缓冲提交失败基础码 */
    CAMERA_RTOS_ERR_IMAGE_PROCESS_BASE = 0x00000400U,  /**< 图像处理失败基础码 */
    CAMERA_RTOS_ERR_DUMP_SEND_BASE = 0x00000500U       /**< 图像发送失败基础码 */
} CameraRtosErrorCode_t;

//============================================================================
// 结构体：RTOS 运行统计信息
//============================================================================

/**
 * @brief  RTOS 运行统计信息结构体
 * @note   记录摄像头服务任务和监控任务的运行计数及错误状态
 */
typedef struct
{
    volatile uint32_t camera_service_loop_count; /**< 摄像头服务主循环运行次数 */
    volatile uint32_t monitor_tick_count;        /**< 监控任务心跳次数 */
    volatile uint32_t cli_command_count;         /**< 完整 CLI 命令次数 */
    volatile uint32_t cli_unknown_count;         /**< 未知 CLI 命令次数 */
    volatile uint32_t dump_request_count;        /**< DUMP 请求次数 */
    volatile uint32_t dump_success_count;        /**< DUMP 成功次数 */
    volatile uint32_t dump_error_count;          /**< DUMP 失败次数 */
    volatile uint32_t uart_none_count;           /**< StreamBuffer 读取超时次数 */
    volatile uint32_t uart_pending_count;        /**< 文本行尚未完整的字节次数 */
    volatile uint32_t uart_error_count;          /**< UART DMA 错误兼容计数 */
    volatile uint32_t binary_request_count;      /**< 合法二进制图像请求次数 */
    volatile uint32_t binary_request_success_count; /**< 二进制请求成功发送图像次数 */
    volatile uint32_t binary_request_error_count;   /**< 二进制请求解析错误总数 */
    volatile uint32_t binary_request_crc_error_count; /**< 二进制请求 CRC 错误次数 */
    volatile uint32_t binary_request_version_error_count; /**< 二进制请求版本错误次数 */
    volatile uint32_t binary_request_type_error_count; /**< 二进制请求类型错误次数 */
    volatile uint32_t binary_request_length_error_count; /**< 二进制请求长度错误次数 */
    volatile uint32_t binary_request_eof_error_count; /**< 二进制请求帧尾错误次数 */
    volatile uint32_t binary_request_timeout_count; /**< 二进制半帧超时次数 */
    volatile uint16_t last_binary_request_seq;  /**< 最近一次合法二进制请求序号 */
    volatile uint32_t last_binary_error_code;   /**< 最近一次二进制解析错误枚举值 */
    volatile uint32_t last_error_code;           /**< 最后一次错误码 */
    volatile uint32_t last_dump_time_ms;         /**< 最近一次 DUMP 耗时 */
    volatile uint32_t last_status_time_ms;       /**< 最近一次 STATUS 时间 */
    volatile uint32_t uptime_ms;                 /**< MonitorTask 累计运行时间 */
} CameraRtosStats_t;

//============================================================================
// RTOS 接口函数
//============================================================================

/**
 * @brief  初始化摄像头 RTOS 模块
 * @param  huart 用于日志输出和 CLI 的 UART 句柄
 * @note   在创建任务之前调用，保存 UART 句柄并重置统计信息
 */
void Camera_RTOS_Init(UART_HandleTypeDef *huart);

/**
 * @brief  记录一条完整 CLI 命令
 */
void Camera_RTOS_RecordCliCommand(void);

/**
 * @brief  记录一条未知 CLI 命令
 */
void Camera_RTOS_RecordCliUnknown(void);

/**
 * @brief  记录一次 DUMP 请求
 */
void Camera_RTOS_RecordDumpRequest(void);

/**
 * @brief  记录一次成功 DUMP
 * @param  elapsed_ms 本次 DUMP 耗时，单位为毫秒
 */
void Camera_RTOS_RecordDumpSuccess(uint32_t elapsed_ms);

/**
 * @brief  记录一次失败 DUMP
 * @param  error_code 本次 DUMP 错误码
 */
void Camera_RTOS_RecordDumpError(uint32_t error_code);

/**
 * @brief  记录一次 UART 无数据状态
 */
void Camera_RTOS_RecordUartNone(void);

/**
 * @brief  记录一次 UART 命令接收中状态
 */
void Camera_RTOS_RecordUartPending(void);

/**
 * @brief  记录一次 UART 接收错误
 */
void Camera_RTOS_RecordUartError(void);

/**
 * @brief  记录 STATUS 命令执行时间
 * @param  time_ms 当前系统时间，单位为毫秒
 */
void Camera_RTOS_RecordStatus(uint32_t time_ms);

/**
 * @brief  摄像头服务任务（FreeRTOS 任务函数）
 * @param  argument 任务参数（未使用）
 * @note   主循环中执行：
 *         1. 从静态 StreamBuffer 分块读取 USART1 RX DMA 数据
 *         2. 将字节送入文本与二进制协议分发器
 *         3. 文本字节继续交给现有 CLI 和 DUMP 解析路径
 *         4. 合法二进制图像请求复用现有 DUMP 发送路径
 *         5. 维护 UART、CLI、DUMP 和二进制请求运行统计
 */
void Camera_RTOS_CameraServiceTask(void *argument);

/**
 * @brief  监控任务（FreeRTOS 任务函数）
 * @param  argument 任务参数（未使用）
 * @note   每 1000 ms 增加心跳计数和累计运行时间，不使用 UART
 */
void Camera_RTOS_MonitorTask(void *argument);

/**
 * @brief  获取运行统计信息的只读指针
 * @return 指向内部静态 @ref CameraRtosStats_t 结构体的指针
 */
const CameraRtosStats_t *Camera_RTOS_GetStats(void);

#endif /* CAMERA_RTOS_H */
