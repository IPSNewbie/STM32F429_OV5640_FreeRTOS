#include "uart_rx_dma.h"  // 循环 DMA、StreamBuffer、恢复状态和统计接口

#include "FreeRTOS.h"      // 提供 BaseType_t 和静态内存相关基础定义
#include "stream_buffer.h" // 提供 ISR 写、任务读的静态 StreamBuffer API
#include "task.h"          // 提供任务临界区，保护溢出状态清理

#include <string.h>         // 提供统计结构和 DMA 缓冲区初始化所需的 memset

//============================================================================
// @file    uart_rx_dma.c
// @brief   USART1 循环 DMA 到 CameraServiceTask 的静态接收通道
//
// 数据路径：circular DMA → HT/TC/IDLE ISR → StreamBuffer → CameraServiceTask。
// DMA 持续写固定环形数组；HAL 回调给出当前写位置，本模块只搬运上次位置之后的
// 新增区间。CameraServiceTask 再以有界阻塞 API 读取，协议解析不在 ISR 中执行。
//
// ISR 只调用 FromISR API、更新轻量统计并设置错误/溢出标志。UART 硬件错误时，
// HAL Abort、清错误位和重启 DMA 延后到任务上下文，避免在中断内执行复杂恢复。
// StreamBuffer 丢字节后协议边界已经不可信，overflow 保持置位，直到任务排空残留
// 并复位上层解析器后才清除。模块全部使用静态存储，不调用 malloc/free。
//============================================================================

/*
 * FreeRTOS StreamBuffer 使用一个空槽区分满和空，因此静态存储区
 * 比对外声明的有效容量多保留一个字节。
 */
// DMA 硬件循环写入的物理缓冲；ISR 根据位置差读取已经完成的区间。
static uint8_t s_uart_rx_dma_buffer[UART_RX_DMA_BUFFER_SIZE];
// StreamBuffer 的静态字节存储，额外 1 字节不计入对外有效容量。
static uint8_t s_uart_rx_stream_storage[UART_RX_STREAM_BUFFER_SIZE + 1U];
// 静态控制块和句柄连接 ISR producer 与 CameraServiceTask consumer。
static StaticStreamBuffer_t s_uart_rx_stream_control;
static StreamBufferHandle_t s_uart_rx_stream;

// 模块绑定的唯一 UART；同时用于过滤其他 UART 的 HAL 回调。
static UART_HandleTypeDef *s_uart_rx_handle;
// 上次已搬运到 StreamBuffer 的 DMA 位置；ISR 更新，任务恢复时归零。
static volatile uint16_t s_old_position;
// StreamBuffer 曾丢字节的粘滞标志，任务完成协议重同步后才允许清除。
static volatile uint8_t s_overflow;
// ISR 只置位恢复请求，CameraServiceTask 看到后执行 HAL Abort/Restart。
static volatile uint8_t s_recovery_required;
// 成功建立静态对象并启动 DMA 后置位，防止未初始化访问。
static volatile uint8_t s_initialized;
// ISR 和任务共同更新的累计统计，STATUS 读取的是实时只读视图。
static UartRxDmaStats_t s_uart_rx_stats;

// 仅在 ISR 中把一个不跨环尾的 DMA 新增区间送入 StreamBuffer。
// partial write 表示缓存空间不足：未写入字节计入 overflow，唤醒标志交由 ISR 末尾处理。
static void UART_RxDma_SendRangeFromISR(uint16_t start,
                                        uint16_t length,
                                        BaseType_t *higher_priority_task_woken)
{
    size_t written;  // FromISR 实际接收的字节数，可能小于请求长度

    if ((length == 0U) ||
        (s_uart_rx_stream == NULL) ||
        (higher_priority_task_woken == NULL))
    {
        return;
    }

    written = xStreamBufferSendFromISR(s_uart_rx_stream,
                                       &s_uart_rx_dma_buffer[start],
                                       (size_t)length,
                                       higher_priority_task_woken);

    s_uart_rx_stats.rx_bytes += (uint32_t)length;
    s_uart_rx_stats.stream_write_bytes += (uint32_t)written;

    if (written < (size_t)length)
    {
        s_uart_rx_stats.stream_overflow_bytes +=
            (uint32_t)((size_t)length - written);
        s_overflow = 1U;
    }
}

// 在任务恢复路径清除 PE/FE/NE/ORE 及 HAL ErrorCode，允许下一次 DMA 正常启动。
static void UART_RxDma_ClearUartErrors(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        return;
    }

    __HAL_UART_CLEAR_PEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);
    huart->ErrorCode = HAL_UART_ERROR_NONE;
}

// 创建静态 StreamBuffer 并启动单 UART 的 Receive-to-IDLE circular DMA。
// 相同句柄重复初始化按幂等成功处理；不同句柄或非 circular DMA 配置被拒绝。
HAL_StatusTypeDef UART_RxDma_Init(UART_HandleTypeDef *huart)
{
    HAL_StatusTypeDef status;  // HAL 启动 Receive-to-IDLE DMA 的结果

    if ((huart == NULL) ||
        (huart->hdmarx == NULL) ||
        (huart->hdmarx->Init.Mode != DMA_CIRCULAR))
    {
        return HAL_ERROR;
    }

    if (s_initialized != 0U)
    {
        return (s_uart_rx_handle == huart) ? HAL_OK : HAL_ERROR;
    }

    (void)memset(&s_uart_rx_stats, 0, sizeof(s_uart_rx_stats));
    (void)memset(s_uart_rx_dma_buffer, 0, sizeof(s_uart_rx_dma_buffer));

    s_uart_rx_stream = xStreamBufferCreateStatic(
        sizeof(s_uart_rx_stream_storage),
        UART_RX_STREAM_TRIGGER_LEVEL,
        s_uart_rx_stream_storage,
        &s_uart_rx_stream_control);
    if (s_uart_rx_stream == NULL)
    {
        return HAL_ERROR;
    }

    s_uart_rx_handle = huart;
    s_old_position = 0U;
    s_overflow = 0U;
    s_recovery_required = 0U;
    s_initialized = 1U;

    status = HAL_UARTEx_ReceiveToIdle_DMA(huart,
                                          s_uart_rx_dma_buffer,
                                          UART_RX_DMA_BUFFER_SIZE);
    if (status != HAL_OK)
    {
        s_initialized = 0U;
        s_uart_rx_handle = NULL;
        return HAL_ERROR;
    }

    return HAL_OK;
}

// 仅由 CameraServiceTask 从 StreamBuffer 有界阻塞读取。
// 返回 0 可能表示超时、无数据、未初始化、参数错误或正在等待 UART 恢复；
// 非零毫秒若换算为 0 tick，会强制至少等待 1 tick，避免意外变成纯轮询。
size_t UART_RxDma_Read(uint8_t *buffer,
                       size_t buffer_size,
                       uint32_t timeout_ms)
{
    TickType_t timeout_ticks;  // FreeRTOS 调度 tick 表示的最长阻塞时间

    if ((buffer == NULL) ||
        (buffer_size == 0U) ||
        (s_uart_rx_stream == NULL) ||
        (s_initialized == 0U) ||
        (s_recovery_required != 0U))
    {
        return 0U;
    }

    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if ((timeout_ms > 0U) && (timeout_ticks == 0U))
    {
        timeout_ticks = 1U;
    }

    return xStreamBufferReceive(s_uart_rx_stream,
                                buffer,
                                buffer_size,
                                timeout_ticks);
}

// 在 HAL HT/TC/IDLE ISR 中按新旧位置差搬运 circular DMA 新增字节。
// size>old 搬 [old,size)；size<old 表示绕回，依次搬 [old,end) 和 [0,size)；
// size==old 没有新数据，尤其避免 TC 后同位置 IDLE 把整环重复写入。
void UART_RxDma_HandleRxEvent(UART_HandleTypeDef *huart, uint16_t size)
{
    BaseType_t higher_priority_task_woken = pdFALSE;  // ISR 末尾是否立即切换到被唤醒任务
    uint16_t old_position;  // 本次回调前最后已搬运的位置

    if ((huart == NULL) ||
        (huart != s_uart_rx_handle) ||
        (s_initialized == 0U))
    {
        return;
    }

    s_uart_rx_stats.rx_event_count++;

    if (size > UART_RX_DMA_BUFFER_SIZE)
    {
        s_uart_rx_stats.uart_error_count++;
        s_recovery_required = 1U;
        return;
    }

    old_position = s_old_position;

    if (size > old_position)
    {
        UART_RxDma_SendRangeFromISR(old_position,
                                    (uint16_t)(size - old_position),
                                    &higher_priority_task_woken);
    }
    else if (size < old_position)
    {
        UART_RxDma_SendRangeFromISR(old_position,
                                    (uint16_t)(UART_RX_DMA_BUFFER_SIZE - old_position),
                                    &higher_priority_task_woken);
        UART_RxDma_SendRangeFromISR(0U,
                                    size,
                                    &higher_priority_task_woken);
    }
    else
    {
        // 位置未变化，不重复写入已经搬运的数据
    }

    /*
     * 保留 BUFFER_SIZE 边界值，可识别 TC 后紧接的同位置 IDLE 事件，
     * 避免把整段 DMA 缓冲区重复写入 StreamBuffer。
     */
    s_old_position = size;
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

// UART Error ISR 只累计错误并置 recovery request，不在中断内调用 HAL Abort/Restart。
void UART_RxDma_HandleError(UART_HandleTypeDef *huart)
{
    if ((huart == NULL) ||
        (huart != s_uart_rx_handle) ||
        (s_initialized == 0U))
    {
        return;
    }

    s_uart_rx_stats.uart_error_count++;
    s_recovery_required = 1U;
}

// 在 CameraServiceTask 中按 Abort→清错误→复位位置→重启 Receive-to-IDLE 的顺序恢复。
// 任一步失败返回 RETRY 并保留请求；本函数不排空 StreamBuffer，也不复位协议 parser，
// 这些边界恢复由 camera_rtos 在确认 DMA 结果后统一完成。
UartRxDmaRecoveryResult_t UART_RxDma_RecoverIfNeeded(void)
{
    HAL_StatusTypeDef status;  // 当前 HAL Abort 或 Restart 步骤的返回值

    if (s_recovery_required == 0U)
    {
        return UART_RX_DMA_RECOVERY_NONE;
    }

    if ((s_uart_rx_handle == NULL) ||
        (s_uart_rx_handle->hdmarx == NULL) ||
        (s_uart_rx_handle->hdmarx->Init.Mode != DMA_CIRCULAR))
    {
        return UART_RX_DMA_RECOVERY_RETRY;
    }

    status = HAL_UART_AbortReceive(s_uart_rx_handle);
    if (status != HAL_OK)
    {
        return UART_RX_DMA_RECOVERY_RETRY;
    }

    UART_RxDma_ClearUartErrors(s_uart_rx_handle);
    s_old_position = 0U;
    s_recovery_required = 0U;

    status = HAL_UARTEx_ReceiveToIdle_DMA(s_uart_rx_handle,
                                          s_uart_rx_dma_buffer,
                                          UART_RX_DMA_BUFFER_SIZE);
    if (status != HAL_OK)
    {
        s_recovery_required = 1U;
        return UART_RX_DMA_RECOVERY_RETRY;
    }

    s_uart_rx_stats.recovery_count++;
    return UART_RX_DMA_RECOVERY_DONE;
}

// 查询粘滞 overflow；置位表示字节流边界已破坏，上层必须先排空并复位解析器。
uint8_t UART_RxDma_HasOverflow(void)
{
    return s_overflow;
}

// 在任务上下文确认 StreamBuffer 已空后清除 overflow。
// 临界区把“检查缓存为空”和“清标志”合成一个操作，避免 ISR 同时写入并重新置位。
void UART_RxDma_ClearOverflow(void)
{
    if (s_uart_rx_stream == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    if ((s_overflow != 0U) &&
        (xStreamBufferBytesAvailable(s_uart_rx_stream) == 0U))
    {
        s_overflow = 0U;
        s_uart_rx_stats.stream_resync_count++;
    }
    taskEXIT_CRITICAL();
}

// 使用 32 字节小栈缓冲非阻塞排空当前 StreamBuffer，供错误/溢出后重建协议边界。
// 每轮 timeout=0，只处理已经存在的数据；某轮收到 0 时退出。缓存容量固定为 512，
// 无并发持续输入时最多 16 个满块轮次，不会永久等待未来字节。
size_t UART_RxDma_Drain(void)
{
    uint8_t discard[32];  // 限制任务栈占用的临时丢弃缓冲
    size_t received;      // 本轮实际取出的字节数，0 表示当前已空
    size_t total = 0U;    // 返回给诊断调用方的累计丢弃量

    if (s_uart_rx_stream == NULL)
    {
        return 0U;
    }

    do
    {
        received = xStreamBufferReceive(s_uart_rx_stream,
                                        discard,
                                        sizeof(discard),
                                        0U);
        total += received;
    } while (received > 0U);

    return total;
}

// 返回内部静态统计的实时只读指针；多字段读取不是锁保护的原子快照。
const UartRxDmaStats_t *UART_RxDma_GetStats(void)
{
    return &s_uart_rx_stats;
}

// HAL 在 HT、TC 或 UART IDLE 中断中调用，统一交给环形位置算法处理。
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    UART_RxDma_HandleRxEvent(huart, size);
}

// HAL UART 错误回调只转交置位请求，实际恢复留给 CameraServiceTask。
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    UART_RxDma_HandleError(huart);
}
