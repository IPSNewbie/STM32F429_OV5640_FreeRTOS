#ifndef UART_RX_DMA_H
#define UART_RX_DMA_H

#include "stm32f4xx_hal.h"  // 提供 UART 句柄、HAL 状态和 Receive-to-IDLE 回调类型

#include <stddef.h>          // 提供 StreamBuffer 读写长度使用的 size_t
#include <stdint.h>          // 提供字节、计数器和毫秒 timeout 的固定宽度类型

/**
 * @file uart_rx_dma.h
 * @brief UART circular DMA、ISR 与 CommTask 之间的静态接收通道
 *
 * 数据依次经过 128 字节 circular DMA、HT/TC/IDLE ISR、512 字节 StreamBuffer，
 * 最终由 CommTask 读取。ISR 只搬运数据和提出恢复请求；HAL Abort/Restart、
 * 协议重同步及缓存排空在任务上下文执行。模块为单 UART、单 reader 设计。
 */

/** @brief DMA 硬件循环写入的 128 字节物理缓冲区大小。 */
#define UART_RX_DMA_BUFFER_SIZE       128U

/** @brief 解耦 ISR 与任务的有效缓存容量；源文件额外 1 字节仅供满/空判定。 */
#define UART_RX_STREAM_BUFFER_SIZE    512U

/** @brief StreamBuffer 唤醒阈值，任意一个字节即可唤醒任务。 */
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
    volatile uint32_t rx_event_count;          /**< ISR：HT/TC/IDLE 事件累计次数 */
    volatile uint32_t rx_bytes;                /**< ISR：DMA 检测出的新增字节数 */
    volatile uint32_t stream_write_bytes;      /**< ISR：实际送入 StreamBuffer 的字节数 */
    volatile uint32_t stream_overflow_bytes;   /**< ISR：因缓存不足丢失的字节数 */
    volatile uint32_t uart_error_count;        /**< ISR：UART 错误或非法 DMA 位置次数 */
    volatile uint32_t recovery_count;          /**< 任务：HAL DMA 恢复成功次数 */
    volatile uint32_t stream_resync_count;     /**< 任务：溢出后确认边界重建次数 */
} UartRxDmaStats_t;

/**
 * @brief 初始化 UART DMA 循环接收和静态 StreamBuffer
 * @param huart 目标 UART 句柄
 * @return HAL_OK 初始化成功，HAL_ERROR 参数或启动失败
 * @note 任务上下文调用；要求 huart->hdmarx 已配置为 DMA_CIRCULAR。相同 UART 可重复调用，
 *       模块不支持运行中切换到另一个 UART，也不使用动态内存。
 */
HAL_StatusTypeDef UART_RxDma_Init(UART_HandleTypeDef *huart);

/**
 * @brief 在任务上下文读取已接收字节
 * @param buffer 目标缓冲区
 * @param buffer_size 目标缓冲区大小
 * @param timeout_ms 最长等待时间，单位为毫秒
 * @return 实际读取字节数；0 也可能表示超时、未初始化、参数非法或正在等待恢复
 * @note 仅限 CommTask 单 reader；ISR 使用 FromISR 写入，不能调用本接口。
 */
size_t UART_RxDma_Read(uint8_t *buffer,
                       size_t buffer_size,
                       uint32_t timeout_ms);

/**
 * @brief 处理 HAL Receive-to-IDLE 事件
 * @param huart 触发事件的 UART 句柄
 * @param size DMA 当前写入位置，允许 0～UART_RX_DMA_BUFFER_SIZE
 * @note ISR context。函数按新旧位置搬运新增区间，不执行协议解析或 HAL 恢复。
 */
void UART_RxDma_HandleRxEvent(UART_HandleTypeDef *huart, uint16_t size);

/**
 * @brief 记录 UART 错误并请求任务上下文恢复
 * @param huart 发生错误的 UART 句柄
 * @note ISR context，只置标志；不能在此调用阻塞或复杂 HAL 恢复 API。
 */
void UART_RxDma_HandleError(UART_HandleTypeDef *huart);

/**
 * @brief 在任务上下文恢复 UART DMA 接收
 * @retval UART_RX_DMA_RECOVERY_NONE 当前没有恢复请求
 * @retval UART_RX_DMA_RECOVERY_DONE 本轮恢复成功，上层应排空并复位协议边界
 * @retval UART_RX_DMA_RECOVERY_RETRY HAL 恢复失败，后续任务循环继续重试
 * @note 仅限任务上下文。
 */
UartRxDmaRecoveryResult_t UART_RxDma_RecoverIfNeeded(void);

/**
 * @brief 查询 StreamBuffer 是否发生过溢出
 * @return 1 表示曾丢字节且需要重同步，0 表示当前没有粘滞溢出
 */
uint8_t UART_RxDma_HasOverflow(void);

/**
 * @brief 在 StreamBuffer 已排空时清除溢出标志
 * @note 仅任务上下文调用。若清除时又有数据到达，则保留标志并继续排空。
 */
void UART_RxDma_ClearOverflow(void);

/**
 * @brief 非阻塞丢弃 StreamBuffer 中的现有数据
 * @return 本次调用非阻塞丢弃的字节数
 * @note 使用固定 32 字节局部缓冲和 0 tick 读取，只排空当前已有数据。
 */
size_t UART_RxDma_Drain(void);

/**
 * @brief 获取 UART DMA 接收统计
 * @return 内部静态统计结构只读指针
 * @note 指针长期有效但字段可被 ISR/任务随时更新，不是原子快照。
 */
const UartRxDmaStats_t *UART_RxDma_GetStats(void);

#endif /* UART_RX_DMA_H */
