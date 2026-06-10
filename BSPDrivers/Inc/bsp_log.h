//
// Created by FAKE on 2026/5/27.
//

#ifndef ISP_OV5640_BSP_LOG_H
#define ISP_OV5640_BSP_LOG_H
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* 日志等级 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE
} LogLevel;

/* 初始化日志模块，传入 CubeMX 生成的串口句柄 */
void log_init(UART_HandleTypeDef *huart1);

/* 设置/获取全局日志等级 */
void log_set_level(LogLevel level);
LogLevel log_get_level(void);

/* 底层输出字符串（阻塞发送） */
void log_output(const char *str);

/* 格式化输出，类似 printf */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 带等级前缀和时间戳的便捷宏 */
#define LOG_DEBUG(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_DEBUG) \
log_printf("[DEBUG][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_INFO) \
log_printf("[INFO][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_WARN) \
log_printf("[WARN][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) \
do { if (log_get_level() <= LOG_LEVEL_ERROR) \
log_printf("[ERROR][%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__); \
} while(0)

/* 原始输出，不带任何前缀 */
#define LOG_RAW(fmt, ...) log_printf(fmt, ##__VA_ARGS__)

#endif //ISP_OV5640_BSP_LOG_H
