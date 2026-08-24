//
// Created by FAKE on 2026/5/27.
//

#ifndef ISP_OV5640_BSP_LOG_H
#define ISP_OV5640_BSP_LOG_H
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/**
 * @brief 日志输出等级
 * @note 等级数值越大，输出越精简；LOG_LEVEL_NONE 关闭分级日志。
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE
} LogLevel;

/**
 * @brief 初始化日志模块
 * @param huart1 CubeMX 生成的日志 UART 句柄
 * @note 本模块保存句柄指针，不负责初始化 UART 外设。
 */
void log_init(UART_HandleTypeDef *huart1);

/**
 * @brief 设置全局日志等级
 * @param level 新的日志等级
 */
void log_set_level(LogLevel level);

/**
 * @brief 获取当前全局日志等级
 * @return 当前日志等级
 */
LogLevel log_get_level(void);

/**
 * @brief 通过日志 UART 阻塞发送字符串
 * @param str 以空字符结尾的待发送字符串；空指针时直接返回
 */
void log_output(const char *str);

/**
 * @brief 按 printf 格式生成并输出日志
 * @param fmt printf 风格格式字符串；空指针时直接返回
 * @note 格式化内容受模块内部固定缓冲区长度限制。
 */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief 输出带毫秒时间戳的 DEBUG 级日志。 */
#define LOG_DEBUG(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_DEBUG) \
log_printf("[DEBUG][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

/** @brief 输出带毫秒时间戳的 INFO 级日志。 */
#define LOG_INFO(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_INFO) \
log_printf("[INFO][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

/** @brief 输出带毫秒时间戳的 WARN 级日志。 */
#define LOG_WARN(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_WARN) \
log_printf("[WARN][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

/** @brief 输出带毫秒时间戳的 ERROR 级日志。 */
#define LOG_ERROR(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_ERROR) \
log_printf("[ERROR][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

/** @brief 原样格式化输出，不附加等级前缀和时间戳。 */
#define LOG_RAW(fmt, ...) log_printf(fmt, ##__VA_ARGS__)

#endif //ISP_OV5640_BSP_LOG_H
