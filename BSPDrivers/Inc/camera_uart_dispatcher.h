#ifndef CAMERA_UART_DISPATCHER_H
#define CAMERA_UART_DISPATCHER_H

#include <stdint.h>

#include "image_request_protocol.h"

// UART 应用层分发器当前模式
typedef enum
{
    CAMERA_UART_DISPATCH_MODE_IDLE = 0,
    CAMERA_UART_DISPATCH_MODE_TEXT,
    CAMERA_UART_DISPATCH_MODE_BINARY
} CameraUartDispatchMode_t;

// 单次分发调用产生的事件类型
typedef enum
{
    CAMERA_UART_DISPATCH_NONE = 0,
    CAMERA_UART_DISPATCH_TEXT_BYTE,
    CAMERA_UART_DISPATCH_IMAGE_REQUEST,
    CAMERA_UART_DISPATCH_BINARY_ERROR,
    CAMERA_UART_DISPATCH_BINARY_TIMEOUT,
    CAMERA_UART_DISPATCH_BAD_ARGUMENT
} CameraUartDispatchResult_t;

// 分发事件数据；仅在对应事件类型下读取相应字段
typedef struct
{
    CameraUartDispatchResult_t type;
    uint8_t text_byte;
    ImageRequestFrame_t image_request;
    ImageRequestParseResult_t binary_result;
} CameraUartDispatchEvent_t;

// 分发器上下文，由调用方静态分配
typedef struct
{
    CameraUartDispatchMode_t mode;
    ImageRequestParser_t binary_parser;
    uint8_t binary_discard_active;      /**< 是否正在隔离错误帧尾部 */
    uint8_t binary_discard_remaining;   /**< 当前错误帧尚需丢弃的字节数 */
    uint32_t binary_discard_last_time_ms; /**< 最近一次隔离字节的时间戳 */
} CameraUartDispatcher_t;

void CameraUartDispatcher_Init(CameraUartDispatcher_t *dispatcher);

void CameraUartDispatcher_Reset(CameraUartDispatcher_t *dispatcher);

CameraUartDispatchResult_t CameraUartDispatcher_FeedByte(
    CameraUartDispatcher_t *dispatcher,
    uint8_t byte,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event);

CameraUartDispatchResult_t CameraUartDispatcher_CheckTimeout(
    CameraUartDispatcher_t *dispatcher,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event);

CameraUartDispatchMode_t CameraUartDispatcher_GetMode(
    const CameraUartDispatcher_t *dispatcher);

#endif /* CAMERA_UART_DISPATCHER_H */
