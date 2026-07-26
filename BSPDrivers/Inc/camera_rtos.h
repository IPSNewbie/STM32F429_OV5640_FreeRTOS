#ifndef CAMERA_RTOS_H
#define CAMERA_RTOS_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

//============================================================================
// @file    camera_rtos.h
// @brief   摄像头 RTOS 任务模块
// @note    定义基于 FreeRTOS 的两个任务：
//          - CameraServiceTask：摄像头服务任务（处理帧采集、PC Dump、CLI）
//          - MonitorTask：监控任务（定期打印统计信息及错误状态）
//          同时提供运行统计信息的获取接口。
//============================================================================

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
    volatile uint32_t dump_success_count;        /**< PC Dump 成功发送帧数 */
    volatile uint32_t dump_error_count;          /**< PC Dump 发送失败次数 */
    volatile uint32_t monitor_tick_count;        /**< 监控任务运行次数（约每秒一次） */
    volatile uint32_t last_error_code;           /**< 最后一次错误码（用于调试） */
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
 * @brief  摄像头服务任务（FreeRTOS 任务函数）
 * @param  argument 任务参数（未使用）
 * @note   主循环中执行：
 *         1. 等待 PC 通过 UART 发送命令（DUMP 或 CLI 命令）
 *         2. 若收到 DUMP，则触发 DCMI 拍照并发送帧数据
 *         3. 若收到 CLI 命令，则通过 Camera_CLI_HandleLine 处理
 *         4. 每循环一次增加 camera_service_loop_count
 */
void Camera_RTOS_CameraServiceTask(void *argument);

/**
 * @brief  监控任务（FreeRTOS 任务函数）
 * @param  argument 任务参数（未使用）
 * @note   周期性（例如每秒）执行：
 *         1. 增加 monitor_tick_count
 *         2. 打印当前统计信息（通过 UART）
 *         3. 若有错误，打印错误码
 */
void Camera_RTOS_MonitorTask(void *argument);

/**
 * @brief  获取运行统计信息的只读指针
 * @return 指向内部静态 @ref CameraRtosStats_t 结构体的指针
 */
const CameraRtosStats_t *Camera_RTOS_GetStats(void);

#endif /* CAMERA_RTOS_H */