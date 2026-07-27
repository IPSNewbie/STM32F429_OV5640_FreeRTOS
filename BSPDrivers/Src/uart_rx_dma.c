#include "uart_rx_dma.h"

#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"

#include <string.h>

/*
 * FreeRTOS StreamBuffer 使用一个空槽区分满和空，因此静态存储区
 * 比对外声明的有效容量多保留一个字节。
 */
static uint8_t s_uart_rx_dma_buffer[UART_RX_DMA_BUFFER_SIZE];
static uint8_t s_uart_rx_stream_storage[UART_RX_STREAM_BUFFER_SIZE + 1U];
static StaticStreamBuffer_t s_uart_rx_stream_control;
static StreamBufferHandle_t s_uart_rx_stream;

static UART_HandleTypeDef *s_uart_rx_handle;
static volatile uint16_t s_old_position;
static volatile uint8_t s_overflow;
static volatile uint8_t s_recovery_required;
static volatile uint8_t s_initialized;
static UartRxDmaStats_t s_uart_rx_stats;

// 将一段 DMA 新增数据写入 StreamBuffer，并记录实际写入量
static void UART_RxDma_SendRangeFromISR(uint16_t start,
                                        uint16_t length,
                                        BaseType_t *higher_priority_task_woken)
{
    size_t written;

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

// 清除可能阻止下一次 DMA 接收的 UART 错误标志
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

HAL_StatusTypeDef UART_RxDma_Init(UART_HandleTypeDef *huart)
{
    HAL_StatusTypeDef status;

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

size_t UART_RxDma_Read(uint8_t *buffer,
                       size_t buffer_size,
                       uint32_t timeout_ms)
{
    TickType_t timeout_ticks;

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

void UART_RxDma_HandleRxEvent(UART_HandleTypeDef *huart, uint16_t size)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    uint16_t old_position;

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

UartRxDmaRecoveryResult_t UART_RxDma_RecoverIfNeeded(void)
{
    HAL_StatusTypeDef status;

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

uint8_t UART_RxDma_HasOverflow(void)
{
    return s_overflow;
}

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

size_t UART_RxDma_Drain(void)
{
    uint8_t discard[32];
    size_t received;
    size_t total = 0U;

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

const UartRxDmaStats_t *UART_RxDma_GetStats(void)
{
    return &s_uart_rx_stats;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    UART_RxDma_HandleRxEvent(huart, size);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    UART_RxDma_HandleError(huart);
}
