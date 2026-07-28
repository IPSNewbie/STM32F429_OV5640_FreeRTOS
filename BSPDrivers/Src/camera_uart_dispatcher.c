#include "camera_uart_dispatcher.h"

#include <stddef.h>

// 清空上一次事件，避免调用方误用残留数据
static void CameraUartDispatcher_ClearEvent(
    CameraUartDispatchEvent_t *event)
{
    event->type = CAMERA_UART_DISPATCH_NONE;
    event->text_byte = 0U;
    event->image_request.version = 0U;
    event->image_request.msg_type = 0U;
    event->image_request.seq = 0U;
    event->image_request.payload_len = 0U;
    event->binary_result = IMAGE_REQUEST_PARSE_NONE;
}

// 清除错误帧尾部隔离状态
static void CameraUartDispatcher_ClearBinaryDiscard(
    CameraUartDispatcher_t *dispatcher)
{
    dispatcher->binary_discard_active = 0U;
    dispatcher->binary_discard_remaining = 0U;
    dispatcher->binary_discard_last_time_ms = 0U;
}

// 根据发生错误前的解析状态计算当前固定帧剩余字节数
static uint8_t CameraUartDispatcher_GetRemainingBytesAfterError(
    ImageRequestParserState_t state_before)
{
    switch (state_before)
    {
        case IMAGE_REQUEST_STATE_VERSION:
            return 11U;

        case IMAGE_REQUEST_STATE_MSG_TYPE:
            return 10U;

        case IMAGE_REQUEST_STATE_LEN_HIGH:
            return 6U;

        case IMAGE_REQUEST_STATE_EOF0:
            return 1U;

        case IMAGE_REQUEST_STATE_EOF1:
        default:
            return 0U;
    }
}

// 字段错误后隔离当前固定帧尚未接收的尾部字节
static void CameraUartDispatcher_StartBinaryDiscard(
    CameraUartDispatcher_t *dispatcher,
    ImageRequestParserState_t state_before,
    uint32_t now_ms)
{
    uint8_t remaining;

    CameraUartDispatcher_ClearBinaryDiscard(dispatcher);

    if (ImageRequestProtocol_IsActive(&dispatcher->binary_parser) != 0U)
    {
        dispatcher->mode = CAMERA_UART_DISPATCH_MODE_BINARY;
        return;
    }

    remaining = CameraUartDispatcher_GetRemainingBytesAfterError(state_before);
    if (remaining == 0U)
    {
        dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
        return;
    }

    dispatcher->binary_discard_active = 1U;
    dispatcher->binary_discard_remaining = remaining;
    dispatcher->binary_discard_last_time_ms = now_ms;
    dispatcher->mode = CAMERA_UART_DISPATCH_MODE_BINARY;
}

// 底层解析后按活动状态更新分发模式
static void CameraUartDispatcher_UpdateBinaryMode(
    CameraUartDispatcher_t *dispatcher)
{
    if (ImageRequestProtocol_IsActive(&dispatcher->binary_parser) != 0U)
    {
        dispatcher->mode = CAMERA_UART_DISPATCH_MODE_BINARY;
    }
    else
    {
        dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
    }
}

// 将图像请求解析结果转换为分发事件
static CameraUartDispatchResult_t CameraUartDispatcher_MapBinaryResult(
    CameraUartDispatcher_t *dispatcher,
    ImageRequestParseResult_t parse_result,
    ImageRequestParserState_t state_before,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event)
{
    if (parse_result == IMAGE_REQUEST_PARSE_BAD_ARGUMENT)
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    switch (parse_result)
    {
        case IMAGE_REQUEST_PARSE_OK:
            CameraUartDispatcher_UpdateBinaryMode(dispatcher);
            out_event->type = CAMERA_UART_DISPATCH_IMAGE_REQUEST;
            out_event->binary_result = parse_result;
            break;

        case IMAGE_REQUEST_PARSE_CRC_ERROR:
            CameraUartDispatcher_UpdateBinaryMode(dispatcher);
            out_event->type = CAMERA_UART_DISPATCH_BINARY_ERROR;
            out_event->binary_result = parse_result;
            break;

        case IMAGE_REQUEST_PARSE_VERSION_ERROR:
        case IMAGE_REQUEST_PARSE_TYPE_ERROR:
        case IMAGE_REQUEST_PARSE_LENGTH_ERROR:
        case IMAGE_REQUEST_PARSE_EOF_ERROR:
            CameraUartDispatcher_StartBinaryDiscard(dispatcher,
                                                     state_before,
                                                     now_ms);
            out_event->type = CAMERA_UART_DISPATCH_BINARY_ERROR;
            out_event->binary_result = parse_result;
            break;

        case IMAGE_REQUEST_PARSE_TIMEOUT:
            CameraUartDispatcher_UpdateBinaryMode(dispatcher);
            out_event->type = CAMERA_UART_DISPATCH_BINARY_TIMEOUT;
            out_event->binary_result = parse_result;
            break;

        case IMAGE_REQUEST_PARSE_NONE:
        case IMAGE_REQUEST_PARSE_PENDING:
        default:
            CameraUartDispatcher_UpdateBinaryMode(dispatcher);
            break;
    }

    return out_event->type;
}

void CameraUartDispatcher_Init(CameraUartDispatcher_t *dispatcher)
{
    if (dispatcher == NULL)
    {
        return;
    }

    ImageRequestProtocol_Init(&dispatcher->binary_parser);
    CameraUartDispatcher_ClearBinaryDiscard(dispatcher);
    dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
}

void CameraUartDispatcher_Reset(CameraUartDispatcher_t *dispatcher)
{
    if (dispatcher == NULL)
    {
        return;
    }

    ImageRequestProtocol_Reset(&dispatcher->binary_parser);
    CameraUartDispatcher_ClearBinaryDiscard(dispatcher);
    dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
}

CameraUartDispatchResult_t CameraUartDispatcher_FeedByte(
    CameraUartDispatcher_t *dispatcher,
    uint8_t byte,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event)
{
    ImageRequestParseResult_t parse_result;
    ImageRequestParserState_t state_before;

    if (out_event != NULL)
    {
        CameraUartDispatcher_ClearEvent(out_event);
    }

    if ((dispatcher == NULL) || (out_event == NULL))
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    if (dispatcher->binary_discard_active != 0U)
    {
        if ((uint32_t)(now_ms - dispatcher->binary_discard_last_time_ms) >=
            IMAGE_REQUEST_TIMEOUT_MS)
        {
            CameraUartDispatcher_ClearBinaryDiscard(dispatcher);
            dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
        }
        else
        {
            if (dispatcher->binary_discard_remaining > 0U)
            {
                dispatcher->binary_discard_remaining--;
            }
            dispatcher->binary_discard_last_time_ms = now_ms;
            if (dispatcher->binary_discard_remaining == 0U)
            {
                CameraUartDispatcher_ClearBinaryDiscard(dispatcher);
                dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
            }
            else
            {
                dispatcher->mode = CAMERA_UART_DISPATCH_MODE_BINARY;
            }
            return CAMERA_UART_DISPATCH_NONE;
        }
    }

    if (dispatcher->mode == CAMERA_UART_DISPATCH_MODE_TEXT)
    {
        out_event->type = CAMERA_UART_DISPATCH_TEXT_BYTE;
        out_event->text_byte = byte;
        dispatcher->mode = (byte == (uint8_t)'\n')
                               ? CAMERA_UART_DISPATCH_MODE_IDLE
                               : CAMERA_UART_DISPATCH_MODE_TEXT;
        return out_event->type;
    }

    if (dispatcher->mode == CAMERA_UART_DISPATCH_MODE_IDLE)
    {
        if (byte != IMAGE_REQUEST_SOF0)
        {
            out_event->type = CAMERA_UART_DISPATCH_TEXT_BYTE;
            out_event->text_byte = byte;
            dispatcher->mode = (byte == (uint8_t)'\n')
                                   ? CAMERA_UART_DISPATCH_MODE_IDLE
                                   : CAMERA_UART_DISPATCH_MODE_TEXT;
            return out_event->type;
        }

        state_before = dispatcher->binary_parser.state;
        parse_result = ImageRequestProtocol_FeedByte(
            &dispatcher->binary_parser,
            byte,
            now_ms,
            &out_event->image_request);
        return CameraUartDispatcher_MapBinaryResult(dispatcher,
                                                    parse_result,
                                                    state_before,
                                                    now_ms,
                                                    out_event);
    }

    if (dispatcher->mode != CAMERA_UART_DISPATCH_MODE_BINARY)
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    state_before = dispatcher->binary_parser.state;
    parse_result = ImageRequestProtocol_FeedByte(
        &dispatcher->binary_parser,
        byte,
        now_ms,
        &out_event->image_request);
    return CameraUartDispatcher_MapBinaryResult(dispatcher,
                                                parse_result,
                                                state_before,
                                                now_ms,
                                                out_event);
}

CameraUartDispatchResult_t CameraUartDispatcher_CheckTimeout(
    CameraUartDispatcher_t *dispatcher,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event)
{
    ImageRequestParseResult_t parse_result;
    ImageRequestParserState_t state_before;

    if (out_event != NULL)
    {
        CameraUartDispatcher_ClearEvent(out_event);
    }

    if ((dispatcher == NULL) || (out_event == NULL))
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    if (dispatcher->binary_discard_active != 0U)
    {
        if ((uint32_t)(now_ms - dispatcher->binary_discard_last_time_ms) >=
            IMAGE_REQUEST_TIMEOUT_MS)
        {
            CameraUartDispatcher_ClearBinaryDiscard(dispatcher);
            dispatcher->mode = CAMERA_UART_DISPATCH_MODE_IDLE;
        }
        else
        {
            dispatcher->mode = CAMERA_UART_DISPATCH_MODE_BINARY;
        }
        return CAMERA_UART_DISPATCH_NONE;
    }

    if ((dispatcher->mode == CAMERA_UART_DISPATCH_MODE_IDLE) ||
        (dispatcher->mode == CAMERA_UART_DISPATCH_MODE_TEXT))
    {
        return CAMERA_UART_DISPATCH_NONE;
    }

    if (dispatcher->mode != CAMERA_UART_DISPATCH_MODE_BINARY)
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    state_before = dispatcher->binary_parser.state;
    parse_result = ImageRequestProtocol_CheckTimeout(
        &dispatcher->binary_parser,
        now_ms);
    return CameraUartDispatcher_MapBinaryResult(dispatcher,
                                                parse_result,
                                                state_before,
                                                now_ms,
                                                out_event);
}

CameraUartDispatchMode_t CameraUartDispatcher_GetMode(
    const CameraUartDispatcher_t *dispatcher)
{
    if (dispatcher == NULL)
    {
        return CAMERA_UART_DISPATCH_MODE_IDLE;
    }

    return dispatcher->mode;
}
