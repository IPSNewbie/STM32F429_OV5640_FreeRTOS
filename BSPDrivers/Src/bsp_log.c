//
// Created by FAKE on 2026/5/27.
//
#include "bsp_log.h"


static UART_HandleTypeDef *log_huart = NULL;
static LogLevel current_level = LOG_LEVEL_DEBUG;   /* 默认输出所有等级 */

/**
 * @brief 初始化日志模块，关联已初始化的串口
 * @param huart1 串口句柄指针，如 &huart1
 */
void log_init(UART_HandleTypeDef *huart1) {
    log_huart = huart1;
}

/**
 * @brief 设置全局日志输出等级
 */
void log_set_level(LogLevel level) {
    current_level = level;
}

/**
 * @brief 获取当前日志输出等级
 */
LogLevel log_get_level(void) {
    return current_level;
}

/**
 * @brief 底层串口发送字符串（阻塞模式）
 * @param str 待发送的字符串（需以 '\0' 结尾）
 */
void log_output(const char *str) {
    if (log_huart == NULL || str == NULL) return;
    HAL_UART_Transmit(log_huart, (uint8_t *)str, strlen(str), HAL_MAX_DELAY);
}

/**
 * @brief 格式化日志输出
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void log_printf(const char *fmt, ...) {
    if (log_huart == NULL) return;
    char buffer[256];   /* 可根据需要调整，注意栈空间 */
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_output(buffer);
}