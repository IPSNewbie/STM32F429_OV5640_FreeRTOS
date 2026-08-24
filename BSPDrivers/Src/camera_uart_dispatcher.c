#include "camera_uart_dispatcher.h"  // UART 文本/二进制分发状态、事件和公开接口

#include <stddef.h>                   // 提供 NULL 空指针常量

//============================================================================
// @file    camera_uart_dispatcher.c
// @brief   USART1 混合字节流的文本命令与 binary image request 分发器
//
// UART DMA/ISR 只把字节送进 StreamBuffer；CommTask 逐字节调用本模块，
// 因而 dispatcher 由单一任务上下文拥有，不在中断中解析协议。
// IDLE 用首字节选择文本或二进制：0xA5 启动请求候选，其他字节进入 TEXT；
// TEXT 保持到 LF；BINARY 把字节交给固定 14 字节图像请求解析器。
//
// 若 version/type/length/EOF 在帧中途出错，后续 CRC/EOF 字节不能立刻成为 CLI。
// 本模块会按出错字段位置隔离旧帧剩余字节，或在 inter-byte timeout 后回到 IDLE。
// 底层解析器若已把当前 0xA5 保留为下一帧起点，则继续 BINARY 而不丢弃新帧。
// 本模块只生成事件，不执行 CLI、图像采集、UART 响应或 CRC 算法。
//============================================================================

// 每次公开入口先清空输出事件，避免 NONE/PENDING 路径遗留上一帧字段。
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

// 清除旧帧尾部隔离计数；底层 binary parser 状态由调用点另行决定是否复位。
static void CameraUartDispatcher_ClearBinaryDiscard(
    CameraUartDispatcher_t *dispatcher)
{
    dispatcher->binary_discard_active = 0U;
    dispatcher->binary_discard_remaining = 0U;
    dispatcher->binary_discard_last_time_ms = 0U;
}

// 根据错误字段在 14 字节帧中的位置计算仍需隔离的尾部长度。
// VERSION/TYPE/LEN_HIGH/EOF0 已消费后分别剩 11/10/6/1 字节；EOF1 后为 0。
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

// 字段错误后隔离旧固定帧尾部，避免残余二进制字节被误当成 CLI 文本。
// 若 Resync 已保留当前 0xA5 作为新 SOF0，则优先继续新候选帧，不能启动旧尾丢弃。
static void CameraUartDispatcher_StartBinaryDiscard(
    CameraUartDispatcher_t *dispatcher,
    ImageRequestParserState_t state_before,
    uint32_t now_ms)
{
    uint8_t remaining;  // 依据出错字段推导的旧帧剩余字节数

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

// 底层解析后按候选帧是否仍 active 选择 BINARY 或 IDLE，保留重同步结果。
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

// 将逐字节解析结果映射为 CommTask 可消费的请求、错误或超时事件。
// CRC 错误发生在完整帧末，无需隔离；字段中途错误必须隔离固定尾部。
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

// 初始化 CommTask 私有的 UART 分发上下文；不启动 DMA，也不清文本行缓冲。
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

// 丢弃二进制候选帧和尾部隔离状态并回到 IDLE；文本解析器由上层单独复位。
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

// 在 CommTask 中消费一个 UART 字节并生成至多一个事件。
// 函数本身没有循环，跨字节状态保存在 dispatcher；TEXT 以 LF 作为模式边界。
CameraUartDispatchResult_t CameraUartDispatcher_FeedByte(
    CameraUartDispatcher_t *dispatcher,
    uint8_t byte,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event)
{
    ImageRequestParseResult_t parse_result;  // 底层 parser 对当前字节的处理结果
    ImageRequestParserState_t state_before;  // 错误前字段位置，用于计算隔离长度

    if (out_event != NULL)
    {
        CameraUartDispatcher_ClearEvent(out_event);
    }

    if ((dispatcher == NULL) || (out_event == NULL))
    {
        return CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    }

    // 未超时时每个输入只递减一次固定尾长；超时则清隔离，让当前字节重新参与 IDLE 判定。
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

    // TEXT 内不识别 0xA5，只原样上交字节；遇到 LF 后下一字节重新分类。
    if (dispatcher->mode == CAMERA_UART_DISPATCH_MODE_TEXT)
    {
        out_event->type = CAMERA_UART_DISPATCH_TEXT_BYTE;
        out_event->text_byte = byte;
        dispatcher->mode = (byte == (uint8_t)'\n')
                               ? CAMERA_UART_DISPATCH_MODE_IDLE
                               : CAMERA_UART_DISPATCH_MODE_TEXT;
        return out_event->type;
    }

    // 只有 IDLE 下的 0xA5 才启动 binary 候选，其余首字节立即属于 CLI 文本。
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

// 在 CommTask 的 UART 有界读取未收到字节时检查 inter-byte timeout。
// discard 超时静默恢复 IDLE；真实候选帧超时则产生可统计的 TIMEOUT 事件。
CameraUartDispatchResult_t CameraUartDispatcher_CheckTimeout(
    CameraUartDispatcher_t *dispatcher,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event)
{
    ImageRequestParseResult_t parse_result;  // 底层候选帧 timeout 检查结果
    ImageRequestParserState_t state_before;  // timeout 前状态，供统一结果映射使用

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

// 只读查询当前分类模式；NULL 返回 IDLE 是容错值，不代表真实对象状态。
CameraUartDispatchMode_t CameraUartDispatcher_GetMode(
    const CameraUartDispatcher_t *dispatcher)
{
    if (dispatcher == NULL)
    {
        return CAMERA_UART_DISPATCH_MODE_IDLE;
    }

    return dispatcher->mode;
}
