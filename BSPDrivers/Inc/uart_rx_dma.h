#ifndef UART_RX_DMA_H
#define UART_RX_DMA_H

#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <stdint.h>

// DMA 循环接收缓冲区大小
#define UART_RX_DMA_BUFFER_SIZE       128U

// StreamBuffer 可保存的有效字节数
#define UART_RX_STREAM_BUFFER_SIZE    512U

// 任意一个字节即可唤醒等待任务
#define UART_RX_STREAM_TRIGGER_LEVEL  1U

/**
 * @brief UART DMA 错误恢复结果
 */
typedef enum
{
    UART_RX_DMA_RECOVERY_NONE = 0, /**< 当前不需要恢复 */
    UART_RX_DMA_RECOVERY_DONE,     /**< 本轮恢复成功 */
    UART_RX_DMA_RECOVERY_RETRY     /**< 本轮恢复失败，需要后续重试 */
} UartRxDmaRecoveryResult_t;

/**
 * @brief UART DMA 接收统计
 */
typedef struct
{
    volatile uint32_t rx_event_count;          /**< RX 事件回调次数 */
    volatile uint32_t rx_bytes;                /**< DMA 检测到的新增字节数 */
    volatile uint32_t stream_write_bytes;      /**< 实际写入 StreamBuffer 的字节数 */
    volatile uint32_t stream_overflow_bytes;   /**< StreamBuffer 未能写入的字节数 */
    volatile uint32_t uart_error_count;        /**< UART 错误回调次数 */
    volatile uint32_t recovery_count;          /**< UART DMA 恢复成功次数 */
    volatile uint32_t stream_resync_count;     /**< StreamBuffer 溢出后重同步次数 */
} UartRxDmaStats_t;

/**
 * @brief 初始化 UART DMA 循环接收和静态 StreamBuffer
 * @param huart 目标 UART 句柄
 * @return HAL_OK 初始化成功，HAL_ERROR 参数或启动失败
 */
HAL_StatusTypeDef UART_RxDma_Init(UART_HandleTypeDef *huart);

/**
 * @brief 在任务上下文读取已接收字节
 * @param buffer 目标缓冲区
 * @param buffer_size 目标缓冲区大小
 * @param timeout_ms 最长等待时间，单位为毫秒
 * @return 实际读取字节数
 */
size_t UART_RxDma_Read(uint8_t *buffer,
                       size_t buffer_size,
                       uint32_t timeout_ms);

/**
 * @brief 处理 HAL Receive-to-IDLE 事件
 * @param huart 触发事件的 UART 句柄
 * @param size DMA 当前写入位置
 */
void UART_RxDma_HandleRxEvent(UART_HandleTypeDef *huart, uint16_t size);

/**
 * @brief 记录 UART 错误并请求任务上下文恢复
 * @param huart 发生错误的 UART 句柄
 */
void UART_RxDma_HandleError(UART_HandleTypeDef *huart);

/**
 * @brief 在任务上下文恢复 UART DMA 接收
 * @return 恢复状态
 */
UartRxDmaRecoveryResult_t UART_RxDma_RecoverIfNeeded(void);

/**
 * @brief 查询 StreamBuffer 是否发生过溢出
 * @return 1 表示需要重同步，0 表示不需要
 */
uint8_t UART_RxDma_HasOverflow(void);

/**
 * @brief 在 StreamBuffer 已排空时清除溢出标志
 * @note 若清除时又有数据到达，则保留标志并由任务继续排空。
 */
void UART_RxDma_ClearOverflow(void);

/**
 * @brief 非阻塞丢弃 StreamBuffer 中的现有数据
 * @return 丢弃的字节数
 */
size_t UART_RxDma_Drain(void);

/**
 * @brief 获取 UART DMA 接收统计
 * @return 内部静态统计结构只读指针
 */
const UartRxDmaStats_t *UART_RxDma_GetStats(void);

#endif /* UART_RX_DMA_H */
