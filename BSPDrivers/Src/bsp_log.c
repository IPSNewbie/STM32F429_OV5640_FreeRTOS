//
// Created by FAKE on 2026/5/27.
//
#include "bsp_log.h"


static UART_HandleTypeDef *log_huart = NULL;
static LogLevel current_level = LOG_LEVEL_DEBUG;   /* 默认输出所有等级 */

// 关联日志模块使用的已初始化 UART
void log_init(UART_HandleTypeDef *huart1) {
    log_huart = huart1;
}

// 设置全局日志输出等级
void log_set_level(LogLevel level) {
    current_level = level;
}

// 获取当前日志输出等级
LogLevel log_get_level(void) {
    return current_level;
}

// 通过 UART 阻塞发送以空字符结尾的日志文本
void log_output(const char *str) {
    if (log_huart == NULL || str == NULL) return;
    HAL_UART_Transmit(log_huart, (uint8_t *)str, strlen(str), HAL_MAX_DELAY);
}

// 在固定长度缓冲区内格式化并输出日志
void log_printf(const char *fmt, ...) {
    if ((log_huart == NULL) || (fmt == NULL)) return;
    char buffer[256];   /* 可根据需要调整，注意栈空间 */
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_output(buffer);
}
