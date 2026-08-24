/*
 * UART 应用层分发器主机侧单元测试。
 *
 * 覆盖文本/二进制复用、错误帧尾部隔离、超时、复位、事件字段清理及连续帧恢复。
 */
#include "camera_uart_dispatcher.h"
#include "protocol_crc32.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXPECTED_TEST_COUNT 85U
#define LONG_TEXT_LENGTH    300U

static uint32_t s_test_total;
static uint32_t s_test_passed;
static uint32_t s_test_failed;

static const uint8_t s_fixed_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x20U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

// 固定错误帧保留完整十四字节，用于验证错误尾部不会泄漏到文本协议
static const uint8_t s_version_error_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x02U, 0x20U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

static const uint8_t s_type_error_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x21U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

static const uint8_t s_length_error_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x20U, 0x34U, 0x12U, 0x01U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

static const uint8_t s_eof0_error_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x20U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x00U, 0x0AU
};

static const uint8_t s_crc_error_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x20U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEBU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

// 构造指定序号的合法二进制图像请求帧
static void BuildValidRequestFrame(
    uint16_t seq,
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE])
{
    uint32_t crc;

    frame[0] = IMAGE_REQUEST_SOF0;
    frame[1] = IMAGE_REQUEST_SOF1;
    frame[2] = IMAGE_REQUEST_VERSION;
    frame[3] = IMAGE_REQUEST_MSG_REQUEST_IMAGE;
    frame[4] = (uint8_t)(seq & 0xFFU);
    frame[5] = (uint8_t)((seq >> 8U) & 0xFFU);
    frame[6] = 0U;
    frame[7] = 0U;

    // CRC32 只覆盖版本、类型、序号和载荷长度六个字节
    crc = Protocol_CRC32_Calculate(&frame[2], 6U);
    frame[8] = (uint8_t)(crc & 0xFFU);
    frame[9] = (uint8_t)((crc >> 8U) & 0xFFU);
    frame[10] = (uint8_t)((crc >> 16U) & 0xFFU);
    frame[11] = (uint8_t)((crc >> 24U) & 0xFFU);
    frame[12] = IMAGE_REQUEST_EOF0;
    frame[13] = IMAGE_REQUEST_EOF1;
}

// 用哨兵值填充事件，便于确认失败路径是否意外改写输出
static void SetEventSentinel(CameraUartDispatchEvent_t *event)
{
    event->type = CAMERA_UART_DISPATCH_BAD_ARGUMENT;
    event->text_byte = 0xE7U;
    event->image_request.version = 0xA1U;
    event->image_request.msg_type = 0xB2U;
    event->image_request.seq = 0xC3D4U;
    event->image_request.payload_len = 0xE5F6U;
    event->binary_result = IMAGE_REQUEST_PARSE_BAD_ARGUMENT;
}

// 检查事件中的图像请求字段是否已清零
static int RequestFieldsAreZero(const CameraUartDispatchEvent_t *event)
{
    return (event->image_request.version == 0U) &&
           (event->image_request.msg_type == 0U) &&
           (event->image_request.seq == 0U) &&
           (event->image_request.payload_len == 0U);
}

// 检查事件是否为字段完整清零的无事件状态
static int EventIsNone(const CameraUartDispatchEvent_t *event)
{
    return (event->type == CAMERA_UART_DISPATCH_NONE) &&
           (event->text_byte == 0U) &&
           RequestFieldsAreZero(event) &&
           (event->binary_result == IMAGE_REQUEST_PARSE_NONE);
}

// 检查事件是否为预期文本字节
static int EventIsText(const CameraUartDispatchEvent_t *event, uint8_t byte)
{
    return (event->type == CAMERA_UART_DISPATCH_TEXT_BYTE) &&
           (event->text_byte == byte) &&
           RequestFieldsAreZero(event) &&
           (event->binary_result == IMAGE_REQUEST_PARSE_NONE);
}

// 检查事件是否为指定序号的合法图像请求
static int EventIsImage(const CameraUartDispatchEvent_t *event, uint16_t seq)
{
    return (event->type == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (event->text_byte == 0U) &&
           (event->image_request.version == IMAGE_REQUEST_VERSION) &&
           (event->image_request.msg_type ==
            IMAGE_REQUEST_MSG_REQUEST_IMAGE) &&
           (event->image_request.seq == seq) &&
           (event->image_request.payload_len ==
            IMAGE_REQUEST_PAYLOAD_LEN_V1) &&
           (event->binary_result == IMAGE_REQUEST_PARSE_OK);
}

// 检查事件是否包含预期二进制解析结果
static int EventIsBinaryResult(
    const CameraUartDispatchEvent_t *event,
    CameraUartDispatchResult_t event_type,
    ImageRequestParseResult_t parse_result)
{
    return (event->type == event_type) &&
           (event->text_byte == 0U) &&
           (event->binary_result == parse_result);
}

// 检查二进制解析器是否已恢复初始状态
static int ParserIsCleared(const ImageRequestParser_t *parser)
{
    return (parser->state == IMAGE_REQUEST_STATE_SYNC0) &&
           (parser->frame.version == 0U) &&
           (parser->frame.msg_type == 0U) &&
           (parser->frame.seq == 0U) &&
           (parser->frame.payload_len == 0U) &&
           (parser->computed_crc == 0U) &&
           (parser->received_crc == 0U) &&
           (parser->last_byte_time_ms == 0U) &&
           (parser->frame_active == 0U);
}

// 检查解析器是否只保留最新帧头起始字节
static int ParserHasFreshSof(
    const ImageRequestParser_t *parser,
    uint32_t expected_time_ms)
{
    return (parser->state == IMAGE_REQUEST_STATE_SYNC1) &&
           (parser->frame.version == 0U) &&
           (parser->frame.msg_type == 0U) &&
           (parser->frame.seq == 0U) &&
           (parser->frame.payload_len == 0U) &&
           (parser->computed_crc == 0U) &&
           (parser->received_crc == 0U) &&
           (parser->last_byte_time_ms == expected_time_ms) &&
           (parser->frame_active == 1U);
}

// 比较两个二进制解析器的全部运行字段
static int ParserMatches(
    const ImageRequestParser_t *left,
    const ImageRequestParser_t *right)
{
    return (left->state == right->state) &&
           (left->frame.version == right->frame.version) &&
           (left->frame.msg_type == right->frame.msg_type) &&
           (left->frame.seq == right->frame.seq) &&
           (left->frame.payload_len == right->frame.payload_len) &&
           (left->computed_crc == right->computed_crc) &&
           (left->received_crc == right->received_crc) &&
           (left->last_byte_time_ms == right->last_byte_time_ms) &&
           (left->frame_active == right->frame_active);
}

// 比较两个 UART 分发器的全部运行字段
static int DispatcherMatches(
    const CameraUartDispatcher_t *left,
    const CameraUartDispatcher_t *right)
{
    return (left->mode == right->mode) &&
           ParserMatches(&left->binary_parser, &right->binary_parser) &&
           (left->binary_discard_active == right->binary_discard_active) &&
           (left->binary_discard_remaining ==
            right->binary_discard_remaining) &&
           (left->binary_discard_last_time_ms ==
            right->binary_discard_last_time_ms);
}

// 连续输入一段数据并返回最后一次分发结果
static CameraUartDispatchResult_t FeedData(
    CameraUartDispatcher_t *dispatcher,
    const uint8_t *data,
    size_t length,
    uint32_t start_time_ms,
    CameraUartDispatchEvent_t *event,
    uint32_t *text_count,
    uint32_t *image_count,
    uint32_t *error_count)
{
    CameraUartDispatchResult_t result = CAMERA_UART_DISPATCH_NONE;
    size_t i;

    for (i = 0U; i < length; ++i)
    {
        result = CameraUartDispatcher_FeedByte(
            dispatcher,
            data[i],
            start_time_ms + (uint32_t)i,
            event);
        if ((result == CAMERA_UART_DISPATCH_TEXT_BYTE) &&
            (text_count != NULL))
        {
            (*text_count)++;
        }
        if ((result == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
            (image_count != NULL))
        {
            (*image_count)++;
        }
        if ((result == CAMERA_UART_DISPATCH_BINARY_ERROR) &&
            (error_count != NULL))
        {
            (*error_count)++;
        }
    }

    return result;
}

// 验证空指针初始化不会访问内存
static int TestInitNull(void)
{
    CameraUartDispatcher_Init(NULL);
    return 1;
}

// 验证空指针复位不会访问内存
static int TestResetNull(void)
{
    CameraUartDispatcher_Reset(NULL);
    return 1;
}

// 验证输入空分发器时返回参数错误并清理事件
static int TestFeedNullDispatcher(void)
{
    CameraUartDispatchEvent_t event;

    SetEventSentinel(&event);
    return (CameraUartDispatcher_FeedByte(
                NULL, 0U, 0U, &event) ==
            CAMERA_UART_DISPATCH_BAD_ARGUMENT) &&
           EventIsNone(&event);
}

// 验证输出事件为空时返回参数错误且不破坏分发器
static int TestFeedNullEvent(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatcher_t before;

    CameraUartDispatcher_Init(&dispatcher);
    before = dispatcher;
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, (uint8_t)'H', 0U, NULL) ==
            CAMERA_UART_DISPATCH_BAD_ARGUMENT) &&
           DispatcherMatches(&dispatcher, &before);
}

// 验证超时检查对空参数的处理
static int TestCheckTimeoutNull(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatcher_t before;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    before = dispatcher;
    SetEventSentinel(&event);
    if ((CameraUartDispatcher_CheckTimeout(NULL, 0U, &event) !=
         CAMERA_UART_DISPATCH_BAD_ARGUMENT) ||
        !EventIsNone(&event))
    {
        return 0;
    }

    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, 0U, NULL) ==
            CAMERA_UART_DISPATCH_BAD_ARGUMENT) &&
           DispatcherMatches(&dispatcher, &before);
}

// 验证初始化后分发器处于完全清零的空闲状态
static int TestInitIdle(void)
{
    CameraUartDispatcher_t dispatcher;

    (void)memset(&dispatcher, 0xA5, sizeof(dispatcher));
    CameraUartDispatcher_Init(&dispatcher);
    return (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE) &&
           ParserIsCleared(&dispatcher.binary_parser) &&
           (dispatcher.binary_discard_active == 0U) &&
           (dispatcher.binary_discard_remaining == 0U) &&
           (dispatcher.binary_discard_last_time_ms == 0U);
}

// 验证复位后分发器恢复空闲状态
static int TestResetIdle(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF1, 1U, &event);
    CameraUartDispatcher_Reset(&dispatcher);
    return (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE) &&
           ParserIsCleared(&dispatcher.binary_parser);
}

// 验证普通文本字节按原值依次分发
static int TestTextBytes(
    const uint8_t *data,
    size_t length,
    CameraUartDispatchMode_t expected_mode)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    size_t i;

    CameraUartDispatcher_Init(&dispatcher);
    for (i = 0U; i < length; ++i)
    {
        if ((CameraUartDispatcher_FeedByte(
                 &dispatcher, data[i], (uint32_t)i, &event) !=
             CAMERA_UART_DISPATCH_TEXT_BYTE) ||
            !EventIsText(&event, data[i]))
        {
            return 0;
        }
    }

    return CameraUartDispatcher_GetMode(&dispatcher) == expected_mode;
}

// 验证回车不会提前结束文本模式
static int TestCrKeepsText(void)
{
    static const uint8_t data[] = {'H', '\r'};

    return TestTextBytes(data,
                         sizeof(data),
                         CAMERA_UART_DISPATCH_MODE_TEXT);
}

// 验证换行结束文本行并恢复空闲模式
static int TestLfEndsText(void)
{
    static const uint8_t data[] = {'H', '\n'};

    return TestTextBytes(data,
                         sizeof(data),
                         CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证空闲状态下的换行按空文本行处理
static int TestEmptyLf(void)
{
    static const uint8_t data[] = {'\n'};

    return TestTextBytes(data,
                         sizeof(data),
                         CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证连续空行不会破坏文本分发状态
static int TestConsecutiveEmptyLines(void)
{
    static const uint8_t data[] = {'\n', '\n', '\n'};

    return TestTextBytes(data,
                         sizeof(data),
                         CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证文本行中的 0xA5 不会误启动二进制解析
static int TestA5InsideText(void)
{
    static const uint8_t data[] = {'H', IMAGE_REQUEST_SOF0};

    return TestTextBytes(data,
                         sizeof(data),
                         CAMERA_UART_DISPATCH_MODE_TEXT);
}

// 验证长文本流保持有界状态且逐字节输出
static int TestLongText(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint32_t i;

    CameraUartDispatcher_Init(&dispatcher);
    for (i = 0U; i < LONG_TEXT_LENGTH; ++i)
    {
        if ((CameraUartDispatcher_FeedByte(
                 &dispatcher, (uint8_t)'X', i, &event) !=
             CAMERA_UART_DISPATCH_TEXT_BYTE) ||
            !EventIsText(&event, (uint8_t)'X') ||
            (CameraUartDispatcher_GetMode(&dispatcher) !=
             CAMERA_UART_DISPATCH_MODE_TEXT))
        {
            return 0;
        }
    }

    return (CameraUartDispatcher_FeedByte(
                &dispatcher, (uint8_t)'\n', LONG_TEXT_LENGTH, &event) ==
            CAMERA_UART_DISPATCH_TEXT_BYTE) &&
           EventIsText(&event, (uint8_t)'\n') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证文本行结束后紧随的 0xA5 可启动二进制候选帧
static int TestLfThenA5(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, (uint8_t)'\n', 0U, &event);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, IMAGE_REQUEST_SOF0, 1U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_BINARY) &&
           ParserHasFreshSof(&dispatcher.binary_parser, 1U);
}

// 输入一帧数据并统计文本、图像和错误事件
static int ParseFrame(
    const uint8_t frame[IMAGE_REQUEST_FRAME_SIZE],
    CameraUartDispatchEvent_t *event,
    CameraUartDispatchMode_t *mode)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchResult_t result;

    CameraUartDispatcher_Init(&dispatcher);
    result = FeedData(&dispatcher,
                      frame,
                      IMAGE_REQUEST_FRAME_SIZE,
                      0U,
                      event,
                      NULL,
                      NULL,
                      NULL);
    *mode = CameraUartDispatcher_GetMode(&dispatcher);
    return result;
}

// 验证固定合法帧只产生一次图像请求事件
static int TestFixedFrameOneImage(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint32_t image_count = 0U;

    CameraUartDispatcher_Init(&dispatcher);
    return (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     0U,
                     &event,
                     NULL,
                     &image_count,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (image_count == 1U);
}

// 验证固定帧序号按小端序解析
static int TestFixedFrameSeq(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (event.image_request.seq == 0x1234U);
}

// 验证固定帧版本字段解析正确
static int TestFixedFrameVersion(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (event.image_request.version == 0x01U);
}

// 验证固定帧消息类型解析正确
static int TestFixedFrameType(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (event.image_request.msg_type == 0x20U);
}

// 验证固定帧载荷长度解析正确
static int TestFixedFrameLength(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (event.image_request.payload_len == 0U);
}

// 验证完整二进制帧不会泄漏文本事件
static int TestFullFrameNoText(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint32_t text_count = 0U;
    uint32_t image_count = 0U;

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   s_fixed_request,
                   sizeof(s_fixed_request),
                   0U,
                   &event,
                   &text_count,
                   &image_count,
                   NULL);
    return (text_count == 0U) && (image_count == 1U);
}

// 验证合法帧完成后分发器恢复空闲
static int TestFrameEndsIdle(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           (mode == CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证指定边界序号能够完整往返解析
static int TestValidSeq(uint16_t seq)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    BuildValidRequestFrame(seq, frame);
    return (ParseFrame(frame, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, seq) &&
           (mode == CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证连续两帧均只产生一次图像事件
static int TestTwoFrames(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t image_count = 0U;

    CameraUartDispatcher_Init(&dispatcher);
    BuildValidRequestFrame(0x1001U, frame);
    (void)FeedData(&dispatcher,
                   frame,
                   sizeof(frame),
                   0U,
                   &event,
                   NULL,
                   &image_count,
                   NULL);
    BuildValidRequestFrame(0x1002U, frame);
    (void)FeedData(&dispatcher,
                   frame,
                   sizeof(frame),
                   20U,
                   &event,
                   NULL,
                   &image_count,
                   NULL);
    return (image_count == 2U) && EventIsImage(&event, 0x1002U) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证二进制请求后可立即解析 HELP 文本
static int TestBinaryThenHelp(void)
{
    static const uint8_t help[] = {'H', 'E', 'L', 'P', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint32_t text_count = 0U;

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   s_fixed_request,
                   sizeof(s_fixed_request),
                   0U,
                   &event,
                   NULL,
                   NULL,
                   NULL);
    (void)FeedData(&dispatcher,
                   help,
                   sizeof(help),
                   20U,
                   &event,
                   &text_count,
                   NULL,
                   NULL);
    return (text_count == 5U) && EventIsText(&event, (uint8_t)'\n') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证 HELP 文本后可立即解析二进制请求
static int TestHelpThenBinary(void)
{
    static const uint8_t help[] = {'H', 'E', 'L', 'P', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   help,
                   sizeof(help),
                   0U,
                   &event,
                   NULL,
                   NULL,
                   NULL);
    return (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     10U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证普通文本结束后可以重新同步二进制帧
static int TestGarbageTextThenBinary(void)
{
    static const uint8_t garbage[] = {0x00U, 0xFFU, 0x5AU, 'X', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint32_t text_count = 0U;

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   garbage,
                   sizeof(garbage),
                   0U,
                   &event,
                   &text_count,
                   NULL,
                   NULL);
    return (text_count == 5U) &&
           (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     10U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证重复 0xA5 后的 0x5A 仍可形成合法帧头
static int TestA5A55ASucceeds(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    return (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     1U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证错误帧头字节回退为空闲且不产生错误事件
static int TestA500(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, 0x00U, 1U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 构造 CRC 字段损坏的完整请求帧
static void BuildCrcErrorFrame(uint8_t frame[IMAGE_REQUEST_FRAME_SIZE])
{
    BuildValidRequestFrame(0x3301U, frame);
    frame[8] ^= 0x01U;
}

// 验证 CRC 错误只产生一次二进制错误事件
static int TestCrcErrorEvent(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    BuildCrcErrorFrame(frame);
    CameraUartDispatcher_Init(&dispatcher);
    return (FeedData(&dispatcher,
                     frame,
                     sizeof(frame),
                     0U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_BINARY_ERROR) &&
           EventIsBinaryResult(&event,
                               CAMERA_UART_DISPATCH_BINARY_ERROR,
                               IMAGE_REQUEST_PARSE_CRC_ERROR);
}

// 验证 CRC 错误帧不会泄漏文本字节
static int TestCrcErrorNoText(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t text_count = 0U;

    BuildCrcErrorFrame(frame);
    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   frame,
                   sizeof(frame),
                   0U,
                   &event,
                   &text_count,
                   NULL,
                   NULL);
    return text_count == 0U;
}

// 验证 CRC 错误帧不会产生图像请求
static int TestCrcErrorNoImage(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t image_count = 0U;

    BuildCrcErrorFrame(frame);
    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   frame,
                   sizeof(frame),
                   0U,
                   &event,
                   NULL,
                   &image_count,
                   NULL);
    return image_count == 0U;
}

// 验证版本、类型或长度等早期字段错误的隔离行为
static int TestEarlyFrameError(
    size_t byte_index,
    uint8_t bad_value,
    size_t feed_length,
    ImageRequestParseResult_t expected_error)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    BuildValidRequestFrame(0x3302U, frame);
    frame[byte_index] = bad_value;
    CameraUartDispatcher_Init(&dispatcher);
    return (FeedData(&dispatcher,
                     frame,
                     feed_length,
                     0U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_BINARY_ERROR) &&
           EventIsBinaryResult(&event,
                               CAMERA_UART_DISPATCH_BINARY_ERROR,
                               expected_error);
}

// 验证错误帧隔离完成后可解析下一合法帧
static int TestErrorThenValid(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t bad_frame[IMAGE_REQUEST_FRAME_SIZE];

    BuildCrcErrorFrame(bad_frame);
    CameraUartDispatcher_Init(&dispatcher);
    if (FeedData(&dispatcher,
                 bad_frame,
                 sizeof(bad_frame),
                 0U,
                 &event,
                 NULL,
                 NULL,
                 NULL) != CAMERA_UART_DISPATCH_BINARY_ERROR)
    {
        return 0;
    }

    return (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     20U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证错误帧隔离完成后可解析下一文本命令
static int TestErrorThenHelp(void)
{
    static const uint8_t help[] = {'H', 'E', 'L', 'P', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t bad_frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t text_count = 0U;

    BuildCrcErrorFrame(bad_frame);
    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   bad_frame,
                   sizeof(bad_frame),
                   0U,
                   &event,
                   NULL,
                   NULL,
                   NULL);
    (void)FeedData(&dispatcher,
                   help,
                   sizeof(help),
                   20U,
                   &event,
                   &text_count,
                   NULL,
                   NULL);
    return (text_count == 5U) && EventIsText(&event, (uint8_t)'\n') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证错误字段值为 0xA5 时不会误同步新帧
static int TestErrorByteA5(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF1, 1U, &event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, IMAGE_REQUEST_SOF0, 2U, &event) ==
            CAMERA_UART_DISPATCH_BINARY_ERROR) &&
           EventIsBinaryResult(&event,
                               CAMERA_UART_DISPATCH_BINARY_ERROR,
                               IMAGE_REQUEST_PARSE_VERSION_ERROR) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_BINARY) &&
           ParserHasFreshSof(&dispatcher.binary_parser, 2U);
}

// 验证帧头同步失败不计为完整二进制帧错误
static int TestHeaderFailureIsNotBinaryError(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, 0x00U, 1U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           (event.type != CAMERA_UART_DISPATCH_BINARY_ERROR) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证空闲状态超时检查不产生事件
static int TestIdleTimeoutNone(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, 100U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证文本模式不受二进制半帧超时影响
static int TestTextTimeoutNone(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, (uint8_t)'H', 0U, &event);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, 1000U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_TEXT);
}

// 验证二进制候选帧在指定时间边界的超时结果
static int TestBinaryTimeoutAt(
    uint32_t elapsed_ms,
    CameraUartDispatchResult_t expected_result,
    CameraUartDispatchMode_t expected_mode)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    SetEventSentinel(&event);
    if ((CameraUartDispatcher_CheckTimeout(
             &dispatcher, 1000U + elapsed_ms, &event) !=
         expected_result) ||
        (CameraUartDispatcher_GetMode(&dispatcher) != expected_mode))
    {
        return 0;
    }

    if (expected_result == CAMERA_UART_DISPATCH_BINARY_TIMEOUT)
    {
        return EventIsBinaryResult(&event,
                                   CAMERA_UART_DISPATCH_BINARY_TIMEOUT,
                                   IMAGE_REQUEST_PARSE_TIMEOUT);
    }

    return EventIsNone(&event);
}

// 验证半帧超时后分发器恢复空闲
static int TestTimeoutEndsIdle(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    (void)CameraUartDispatcher_CheckTimeout(&dispatcher, 1100U, &event);
    return (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE) &&
           ParserIsCleared(&dispatcher.binary_parser);
}

// 验证输入新字节时会先报告已到期候选帧
static int TestFeedReportsOldTimeout(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, (uint8_t)'H', 1100U, &event) ==
            CAMERA_UART_DISPATCH_BINARY_TIMEOUT) &&
           EventIsBinaryResult(&event,
                               CAMERA_UART_DISPATCH_BINARY_TIMEOUT,
                               IMAGE_REQUEST_PARSE_TIMEOUT) &&
           (event.type != CAMERA_UART_DISPATCH_TEXT_BYTE);
}

// 验证超时后的当前 0xA5 可作为新帧起点
static int TestTimeoutCurrentA5(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, IMAGE_REQUEST_SOF0, 1100U, &event) ==
            CAMERA_UART_DISPATCH_BINARY_TIMEOUT) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_BINARY) &&
           ParserHasFreshSof(&dispatcher.binary_parser, 1100U);
}

// 验证毫秒计数回绕时的超时计算
static int TestTickWrapTimeout(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    const uint32_t start_time_ms = 0xFFFFFFF0U;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, start_time_ms, &event);
    if ((CameraUartDispatcher_CheckTimeout(
             &dispatcher,
             (uint32_t)(start_time_ms + 99U),
             &event) != CAMERA_UART_DISPATCH_NONE) ||
        (CameraUartDispatcher_GetMode(&dispatcher) !=
         CAMERA_UART_DISPATCH_MODE_BINARY))
    {
        return 0;
    }

    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher,
                (uint32_t)(start_time_ms + 100U),
                &event) == CAMERA_UART_DISPATCH_BINARY_TIMEOUT) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证半帧超时后可继续解析文本
static int TestTimeoutThenText(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    (void)CameraUartDispatcher_CheckTimeout(&dispatcher, 1100U, &event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, (uint8_t)'H', 1101U, &event) ==
            CAMERA_UART_DISPATCH_TEXT_BYTE) &&
           EventIsText(&event, (uint8_t)'H') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_TEXT);
}

// 验证半帧超时后可继续解析完整二进制帧
static int TestTimeoutThenBinary(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 1000U, &event);
    (void)CameraUartDispatcher_CheckTimeout(&dispatcher, 1100U, &event);
    return (FeedData(&dispatcher,
                     s_fixed_request,
                     sizeof(s_fixed_request),
                     1101U,
                     &event,
                     NULL,
                     NULL,
                     NULL) == CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证无事件结果会清除旧请求字段
static int TestNoneClearsRequestFields(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           RequestFieldsAreZero(&event);
}

// 验证文本事件仅填充文本字节字段
static int TestTextByteField(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    return (CameraUartDispatcher_FeedByte(
                &dispatcher, (uint8_t)'Q', 0U, &event) ==
            CAMERA_UART_DISPATCH_TEXT_BYTE) &&
           (event.text_byte == (uint8_t)'Q');
}

// 验证图像请求事件填充全部业务字段
static int TestImageRequestFields(void)
{
    CameraUartDispatchEvent_t event;
    CameraUartDispatchMode_t mode;

    return (ParseFrame(s_fixed_request, &event, &mode) ==
            CAMERA_UART_DISPATCH_IMAGE_REQUEST) &&
           EventIsImage(&event, 0x1234U);
}

// 验证二进制错误事件保留具体解析错误码
static int TestBinaryErrorResultField(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    BuildCrcErrorFrame(frame);
    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   frame,
                   sizeof(frame),
                   0U,
                   &event,
                   NULL,
                   NULL,
                   NULL);
    return (event.type == CAMERA_UART_DISPATCH_BINARY_ERROR) &&
           (event.binary_result == IMAGE_REQUEST_PARSE_CRC_ERROR);
}

// 验证超时事件写入对应二进制结果字段
static int TestTimeoutResultField(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)CameraUartDispatcher_FeedByte(
        &dispatcher, IMAGE_REQUEST_SOF0, 0U, &event);
    (void)CameraUartDispatcher_CheckTimeout(&dispatcher, 100U, &event);
    return (event.type == CAMERA_UART_DISPATCH_BINARY_TIMEOUT) &&
           (event.binary_result == IMAGE_REQUEST_PARSE_TIMEOUT);
}

// 验证复位会清除上一次请求内容
static int TestResetRemovesOldRequest(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedData(&dispatcher,
                   s_fixed_request,
                   sizeof(s_fixed_request),
                   0U,
                   &event,
                   NULL,
                   NULL,
                   NULL);
    if (!EventIsImage(&event, 0x1234U))
    {
        return 0;
    }

    CameraUartDispatcher_Reset(&dispatcher);
    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, 100U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event);
}

// 验证后续无事件调用不会重复上一次事件
static int TestNoRepeatedOldEvent(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;

    CameraUartDispatcher_Init(&dispatcher);
    if ((FeedData(&dispatcher,
                  s_fixed_request,
                  sizeof(s_fixed_request),
                  0U,
                  &event,
                  NULL,
                  NULL,
                  NULL) != CAMERA_UART_DISPATCH_IMAGE_REQUEST) ||
        !EventIsImage(&event, 0x1234U))
    {
        return 0;
    }

    if ((CameraUartDispatcher_FeedByte(
             &dispatcher, (uint8_t)'\n', 20U, &event) !=
         CAMERA_UART_DISPATCH_TEXT_BYTE) ||
        !EventIsText(&event, (uint8_t)'\n'))
    {
        return 0;
    }

    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, 21U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event);
}

typedef struct
{
    uint32_t text_count;
    uint32_t image_count;
    uint32_t error_count;
    uint32_t timeout_count;
    ImageRequestParseResult_t last_error_result;
} DispatchEventCounts_t;

// 按事件类型累计一次分发结果
static void CountDispatchResult(
    CameraUartDispatchResult_t result,
    const CameraUartDispatchEvent_t *event,
    DispatchEventCounts_t *counts)
{
    if (result == CAMERA_UART_DISPATCH_TEXT_BYTE)
    {
        counts->text_count++;
    }
    else if (result == CAMERA_UART_DISPATCH_IMAGE_REQUEST)
    {
        counts->image_count++;
    }
    else if (result == CAMERA_UART_DISPATCH_BINARY_ERROR)
    {
        counts->error_count++;
        counts->last_error_result = event->binary_result;
    }
    else if (result == CAMERA_UART_DISPATCH_BINARY_TIMEOUT)
    {
        counts->timeout_count++;
    }
    else
    {
        // NONE 和参数错误均不属于业务事件计数
    }
}

// 连续输入数据并累计各类分发事件
static CameraUartDispatchResult_t FeedAndCountEvents(
    CameraUartDispatcher_t *dispatcher,
    const uint8_t *data,
    size_t length,
    uint32_t start_time_ms,
    CameraUartDispatchEvent_t *event,
    DispatchEventCounts_t *counts)
{
    CameraUartDispatchResult_t result = CAMERA_UART_DISPATCH_NONE;
    size_t i;

    for (i = 0U; i < length; ++i)
    {
        result = CameraUartDispatcher_FeedByte(
            dispatcher,
            data[i],
            start_time_ms + (uint32_t)i,
            event);
        CountDispatchResult(result, event, counts);
    }

    return result;
}

// 验证每个完整错误帧只报告一次错误
static int TestCompleteErrorOnce(
    const uint8_t frame[IMAGE_REQUEST_FRAME_SIZE],
    ImageRequestParseResult_t expected_error)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedAndCountEvents(&dispatcher,
                             frame,
                             IMAGE_REQUEST_FRAME_SIZE,
                             0U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.image_count == 0U) &&
           (counts.last_error_result == expected_error) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证完整错误帧隔离期间不产生文本事件
static int TestCompleteErrorHasNoText(
    const uint8_t frame[IMAGE_REQUEST_FRAME_SIZE],
    ImageRequestParseResult_t expected_error)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedAndCountEvents(&dispatcher,
                             frame,
                             IMAGE_REQUEST_FRAME_SIZE,
                             0U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.text_count == 0U) &&
           (counts.last_error_result == expected_error);
}

// 验证指定错误帧后文本命令仍可恢复
static int TestBadFrameThenText(
    const uint8_t bad_frame[IMAGE_REQUEST_FRAME_SIZE],
    ImageRequestParseResult_t expected_error,
    const uint8_t *text,
    size_t text_length)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedAndCountEvents(&dispatcher,
                             bad_frame,
                             IMAGE_REQUEST_FRAME_SIZE,
                             0U,
                             &event,
                             &counts);
    (void)FeedAndCountEvents(&dispatcher,
                             text,
                             text_length,
                             20U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.text_count == (uint32_t)text_length) &&
           (counts.image_count == 0U) &&
           (counts.timeout_count == 0U) &&
           (counts.last_error_result == expected_error) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证指定错误帧后合法二进制帧仍可恢复
static int TestBadFrameThenBinary(
    const uint8_t bad_frame[IMAGE_REQUEST_FRAME_SIZE],
    ImageRequestParseResult_t expected_error)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};

    CameraUartDispatcher_Init(&dispatcher);
    (void)FeedAndCountEvents(&dispatcher,
                             bad_frame,
                             IMAGE_REQUEST_FRAME_SIZE,
                             0U,
                             &event,
                             &counts);
    (void)FeedAndCountEvents(&dispatcher,
                             s_fixed_request,
                             IMAGE_REQUEST_FRAME_SIZE,
                             20U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.image_count == 1U) &&
           (counts.text_count == 0U) &&
           (counts.timeout_count == 0U) &&
           (counts.last_error_result == expected_error) &&
           EventIsImage(&event, 0x1234U) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 构造版本错误后尚未丢完帧尾的隔离状态
static int EnterPartialVersionDiscard(
    CameraUartDispatcher_t *dispatcher,
    CameraUartDispatchEvent_t *event,
    DispatchEventCounts_t *counts,
    uint32_t *last_discard_time_ms)
{
    CameraUartDispatcher_Init(dispatcher);
    (void)FeedAndCountEvents(dispatcher,
                             s_version_error_request,
                             3U,
                             0U,
                             event,
                             counts);
    (void)FeedAndCountEvents(dispatcher,
                             &s_version_error_request[3],
                             2U,
                             3U,
                             event,
                             counts);
    *last_discard_time_ms = 4U;

    return (counts->error_count == 1U) &&
           (counts->text_count == 0U) &&
           (counts->image_count == 0U) &&
           (dispatcher->binary_discard_active == 1U) &&
           (dispatcher->binary_discard_remaining == 9U) &&
           (dispatcher->binary_discard_last_time_ms ==
            *last_discard_time_ms) &&
           (CameraUartDispatcher_GetMode(dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_BINARY);
}

// 验证错误尾部隔离在 99 ms 时仍保持有效
static int TestDiscardAt99Ms(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, last_time_ms + 99U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (dispatcher.binary_discard_active == 1U) &&
           (dispatcher.binary_discard_remaining == 9U) &&
           (dispatcher.binary_discard_last_time_ms == last_time_ms) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_BINARY);
}

// 验证错误尾部隔离在 100 ms 边界结束
static int TestDiscardAt100Ms(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    SetEventSentinel(&event);
    return (CameraUartDispatcher_CheckTimeout(
                &dispatcher, last_time_ms + 100U, &event) ==
            CAMERA_UART_DISPATCH_NONE) &&
           EventIsNone(&event) &&
           (dispatcher.binary_discard_active == 0U) &&
           (dispatcher.binary_discard_remaining == 0U) &&
           (dispatcher.binary_discard_last_time_ms == 0U) &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证隔离超时不会重复报告原二进制错误
static int TestDiscardTimeoutNoSecondError(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    CameraUartDispatchResult_t result;
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    result = CameraUartDispatcher_CheckTimeout(
        &dispatcher, last_time_ms + 100U, &event);
    CountDispatchResult(result, &event, &counts);
    return (result == CAMERA_UART_DISPATCH_NONE) &&
           (counts.error_count == 1U) &&
           (event.type != CAMERA_UART_DISPATCH_BINARY_ERROR);
}

// 验证错误尾部隔离结束不伪造半帧超时事件
static int TestDiscardTimeoutNoTimeoutEvent(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    CameraUartDispatchResult_t result;
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    result = CameraUartDispatcher_CheckTimeout(
        &dispatcher, last_time_ms + 100U, &event);
    CountDispatchResult(result, &event, &counts);
    return (result == CAMERA_UART_DISPATCH_NONE) &&
           (counts.timeout_count == 0U) &&
           (event.type != CAMERA_UART_DISPATCH_BINARY_TIMEOUT) &&
           (event.binary_result == IMAGE_REQUEST_PARSE_NONE);
}

// 验证错误尾部隔离超时后可解析文本
static int TestDiscardTimeoutThenText(void)
{
    static const uint8_t help[] = {'H', 'E', 'L', 'P', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    // 首个新文本字节到达时同时解除已超时的尾部隔离
    (void)FeedAndCountEvents(&dispatcher,
                             help,
                             sizeof(help),
                             last_time_ms + 100U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.text_count == 5U) &&
           (counts.image_count == 0U) &&
           (counts.timeout_count == 0U) &&
           EventIsText(&event, (uint8_t)'\n') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证错误尾部隔离超时后可解析二进制帧
static int TestDiscardTimeoutThenBinary(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    (void)CameraUartDispatcher_CheckTimeout(
        &dispatcher, last_time_ms + 100U, &event);
    (void)FeedAndCountEvents(&dispatcher,
                             s_fixed_request,
                             IMAGE_REQUEST_FRAME_SIZE,
                             last_time_ms + 101U,
                             &event,
                             &counts);
    return (counts.error_count == 1U) &&
           (counts.image_count == 1U) &&
           (counts.text_count == 0U) &&
           (counts.timeout_count == 0U) &&
           EventIsImage(&event, 0x1234U);
}

// 验证隔离期间复位可立即恢复空闲
static int TestResetDuringDiscard(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    CameraUartDispatcher_Reset(&dispatcher);
    return (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE) &&
           (dispatcher.binary_discard_active == 0U);
}

// 验证复位清除全部错误尾部隔离字段
static int TestResetClearsDiscardFields(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    CameraUartDispatcher_Reset(&dispatcher);
    return (dispatcher.binary_discard_active == 0U) &&
           (dispatcher.binary_discard_remaining == 0U) &&
           (dispatcher.binary_discard_last_time_ms == 0U) &&
           ParserIsCleared(&dispatcher.binary_parser);
}

// 验证隔离期间复位后可解析文本
static int TestResetDiscardThenText(void)
{
    static const uint8_t help[] = {'H', 'E', 'L', 'P', '\n'};
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    CameraUartDispatcher_Reset(&dispatcher);
    counts = (DispatchEventCounts_t){0};
    (void)FeedAndCountEvents(&dispatcher,
                             help,
                             sizeof(help),
                             10U,
                             &event,
                             &counts);
    return (counts.text_count == 5U) &&
           (counts.error_count == 0U) &&
           EventIsText(&event, (uint8_t)'\n') &&
           (CameraUartDispatcher_GetMode(&dispatcher) ==
            CAMERA_UART_DISPATCH_MODE_IDLE);
}

// 验证隔离期间复位后可解析二进制帧
static int TestResetDiscardThenBinary(void)
{
    CameraUartDispatcher_t dispatcher;
    CameraUartDispatchEvent_t event;
    DispatchEventCounts_t counts = {0};
    uint32_t last_time_ms;

    if (!EnterPartialVersionDiscard(&dispatcher,
                                    &event,
                                    &counts,
                                    &last_time_ms))
    {
        return 0;
    }

    CameraUartDispatcher_Reset(&dispatcher);
    counts = (DispatchEventCounts_t){0};
    (void)FeedAndCountEvents(&dispatcher,
                             s_fixed_request,
                             IMAGE_REQUEST_FRAME_SIZE,
                             10U,
                             &event,
                             &counts);
    return (counts.image_count == 1U) &&
           (counts.error_count == 0U) &&
           (counts.text_count == 0U) &&
           EventIsImage(&event, 0x1234U);
}

// 记录单项测试结果并输出失败名称
static void RunTest(const char *name, int passed)
{
    (void)name;
    s_test_total++;
    if (passed != 0)
    {
        s_test_passed++;
    }
    else
    {
        s_test_failed++;
    }
}

// 运行 UART 分发器全部主机侧单元测试
int main(void)
{
    static const uint8_t help_lf[] = {'H', 'E', 'L', 'P', '\n'};
    static const uint8_t help_crlf[] = {'H', 'E', 'L', 'P', '\r', '\n'};

    RunTest("A1 Init空指针安全", TestInitNull());
    RunTest("A2 Reset空指针安全", TestResetNull());
    RunTest("A3 GetMode空指针返回空闲",
            CameraUartDispatcher_GetMode(NULL) ==
                CAMERA_UART_DISPATCH_MODE_IDLE);
    RunTest("A4 FeedByte空分发器返回参数错误",
            TestFeedNullDispatcher());
    RunTest("A5 FeedByte空事件返回参数错误", TestFeedNullEvent());
    RunTest("A6 CheckTimeout空参数返回参数错误",
            TestCheckTimeoutNull());
    RunTest("A7 初始化后处于空闲模式", TestInitIdle());
    RunTest("A8 重置后处于空闲模式", TestResetIdle());

    RunTest("B9 HELP换行五字节全部为文本事件",
            TestTextBytes(help_lf,
                          sizeof(help_lf),
                          CAMERA_UART_DISPATCH_MODE_IDLE));
    RunTest("B10 HELP完成后返回空闲模式",
            TestTextBytes(help_lf,
                          sizeof(help_lf),
                          CAMERA_UART_DISPATCH_MODE_IDLE));
    RunTest("B11 HELP回车换行六字节全部为文本事件",
            TestTextBytes(help_crlf,
                          sizeof(help_crlf),
                          CAMERA_UART_DISPATCH_MODE_IDLE));
    RunTest("B12 回车后保持文本模式", TestCrKeepsText());
    RunTest("B13 换行后返回空闲模式", TestLfEndsText());
    RunTest("B14 空换行产生文本事件并保持空闲", TestEmptyLf());
    RunTest("B15 连续空行均正常分发", TestConsecutiveEmptyLines());
    RunTest("B16 文本中的A5仍是文本字节", TestA5InsideText());
    RunTest("B17 超长文本在换行前保持文本模式", TestLongText());
    RunTest("B18 换行后的A5启动二进制候选", TestLfThenA5());

    RunTest("C19 固定请求帧只产生一次图像请求",
            TestFixedFrameOneImage());
    RunTest("C20 固定请求帧序号为1234", TestFixedFrameSeq());
    RunTest("C21 固定请求帧版本为01", TestFixedFrameVersion());
    RunTest("C22 固定请求帧类型为20", TestFixedFrameType());
    RunTest("C23 固定请求帧载荷长度为0", TestFixedFrameLength());
    RunTest("C24 完整二进制帧不产生文本事件", TestFullFrameNoText());
    RunTest("C25 完整二进制帧结束后返回空闲", TestFrameEndsIdle());
    RunTest("C26 序号0000请求帧", TestValidSeq(0x0000U));
    RunTest("C27 序号FFFF请求帧", TestValidSeq(0xFFFFU));
    RunTest("C28 连续两帧产生两次图像请求", TestTwoFrames());
    RunTest("C29 二进制帧后可接收HELP", TestBinaryThenHelp());
    RunTest("C30 HELP后可接收二进制帧", TestHelpThenBinary());

    RunTest("D31 垃圾文本换行后可接收二进制帧",
            TestGarbageTextThenBinary());
    RunTest("D32 A5 A5 5A快速重同步后成功", TestA5A55ASucceeds());
    RunTest("D33 A5 00静默返回空闲", TestA500());
    RunTest("D34 CRC错误产生二进制错误事件", TestCrcErrorEvent());
    RunTest("D35 CRC错误帧不产生文本事件", TestCrcErrorNoText());
    RunTest("D36 CRC错误帧不产生图像请求", TestCrcErrorNoImage());
    RunTest("D37 版本错误事件",
            TestEarlyFrameError(2U,
                                0x02U,
                                3U,
                                IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("D38 类型错误事件",
            TestEarlyFrameError(3U,
                                0x21U,
                                4U,
                                IMAGE_REQUEST_PARSE_TYPE_ERROR));
    RunTest("D39 长度错误事件",
            TestEarlyFrameError(6U,
                                0x01U,
                                8U,
                                IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("D40 EOF0错误事件",
            TestEarlyFrameError(12U,
                                0x00U,
                                13U,
                                IMAGE_REQUEST_PARSE_EOF_ERROR));
    RunTest("D41 EOF1错误事件",
            TestEarlyFrameError(13U,
                                0x00U,
                                IMAGE_REQUEST_FRAME_SIZE,
                                IMAGE_REQUEST_PARSE_EOF_ERROR));
    RunTest("D42 错误帧后可接收合法二进制帧", TestErrorThenValid());
    RunTest("D43 错误帧后可接收HELP", TestErrorThenHelp());
    RunTest("D44 错误字节A5保留二进制候选", TestErrorByteA5());
    RunTest("D45 普通帧头失败不产生二进制错误",
            TestHeaderFailureIsNotBinaryError());

    RunTest("E46 空闲模式超时检查返回无事件", TestIdleTimeoutNone());
    RunTest("E47 文本模式超时检查返回无事件", TestTextTimeoutNone());
    RunTest("E48 二进制候选99毫秒不超时",
            TestBinaryTimeoutAt(99U,
                                CAMERA_UART_DISPATCH_NONE,
                                CAMERA_UART_DISPATCH_MODE_BINARY));
    RunTest("E49 二进制候选100毫秒超时",
            TestBinaryTimeoutAt(100U,
                                CAMERA_UART_DISPATCH_BINARY_TIMEOUT,
                                CAMERA_UART_DISPATCH_MODE_IDLE));
    RunTest("E50 超时后返回空闲模式", TestTimeoutEndsIdle());
    RunTest("E51 FeedByte上报旧候选超时", TestFeedReportsOldTimeout());
    RunTest("E52 超时当前字节A5保留二进制候选",
            TestTimeoutCurrentA5());
    RunTest("E53 时间戳回绕超时判断", TestTickWrapTimeout());
    RunTest("E54 超时后可接收文本", TestTimeoutThenText());
    RunTest("E55 超时后可接收二进制帧", TestTimeoutThenBinary());

    RunTest("F56 NONE事件清零请求字段", TestNoneClearsRequestFields());
    RunTest("F57 文本事件携带正确字节", TestTextByteField());
    RunTest("F58 图像事件携带完整请求字段", TestImageRequestFields());
    RunTest("F59 二进制错误事件携带底层结果",
            TestBinaryErrorResultField());
    RunTest("F60 超时事件携带TIMEOUT结果", TestTimeoutResultField());
    RunTest("F61 Reset后不保留旧请求事件", TestResetRemovesOldRequest());
    RunTest("F62 连续调用不重复旧事件", TestNoRepeatedOldEvent());

    RunTest("G63 完整版本错误帧只产生一次错误",
            TestCompleteErrorOnce(s_version_error_request,
                                  IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("G64 版本错误帧剩余十一字节无文本事件",
            TestCompleteErrorHasNoText(s_version_error_request,
                                       IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("G65 完整类型错误帧只产生一次错误",
            TestCompleteErrorOnce(s_type_error_request,
                                  IMAGE_REQUEST_PARSE_TYPE_ERROR));
    RunTest("G66 类型错误帧剩余十字节无文本事件",
            TestCompleteErrorHasNoText(s_type_error_request,
                                       IMAGE_REQUEST_PARSE_TYPE_ERROR));
    RunTest("G67 完整长度错误帧只产生一次错误",
            TestCompleteErrorOnce(s_length_error_request,
                                  IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("G68 长度错误帧剩余六字节无文本事件",
            TestCompleteErrorHasNoText(s_length_error_request,
                                       IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("G69 EOF0错误帧最后一字节无文本事件",
            TestCompleteErrorHasNoText(s_eof0_error_request,
                                       IMAGE_REQUEST_PARSE_EOF_ERROR));
    RunTest("G70 CRC错误完整帧无文本事件",
            TestCompleteErrorHasNoText(s_crc_error_request,
                                       IMAGE_REQUEST_PARSE_CRC_ERROR));

    RunTest("H71 完整版本错误帧后HELP仅产生五个文本事件",
            TestBadFrameThenText(s_version_error_request,
                                 IMAGE_REQUEST_PARSE_VERSION_ERROR,
                                 help_lf,
                                 sizeof(help_lf)));
    RunTest("H72 完整类型错误帧后HELP回车换行仅产生六个文本事件",
            TestBadFrameThenText(s_type_error_request,
                                 IMAGE_REQUEST_PARSE_TYPE_ERROR,
                                 help_crlf,
                                 sizeof(help_crlf)));
    RunTest("H73 完整长度错误帧后合法二进制帧正常",
            TestBadFrameThenBinary(s_length_error_request,
                                   IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("H74 完整版本错误帧后合法二进制帧正常",
            TestBadFrameThenBinary(s_version_error_request,
                                   IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("H75 完整EOF0错误帧后合法二进制帧正常",
            TestBadFrameThenBinary(s_eof0_error_request,
                                   IMAGE_REQUEST_PARSE_EOF_ERROR));

    RunTest("I76 截断错误尾部经过九十九毫秒仍隔离",
            TestDiscardAt99Ms());
    RunTest("I77 截断错误尾部经过一百毫秒解除隔离",
            TestDiscardAt100Ms());
    RunTest("I78 尾部隔离超时不重复产生错误",
            TestDiscardTimeoutNoSecondError());
    RunTest("I79 尾部隔离超时不产生二进制超时事件",
            TestDiscardTimeoutNoTimeoutEvent());
    RunTest("I80 尾部隔离超时后HELP正常",
            TestDiscardTimeoutThenText());
    RunTest("I81 尾部隔离超时后合法二进制帧正常",
            TestDiscardTimeoutThenBinary());

    RunTest("J82 尾部隔离中Reset立即返回空闲",
            TestResetDuringDiscard());
    RunTest("J83 Reset清零全部尾部隔离字段",
            TestResetClearsDiscardFields());
    RunTest("J84 Reset后文本命令正常", TestResetDiscardThenText());
    RunTest("J85 Reset后二进制请求正常", TestResetDiscardThenBinary());

    if (s_test_total != EXPECTED_TEST_COUNT)
    {
        return 1;
    }

    if ((s_test_passed == EXPECTED_TEST_COUNT) && (s_test_failed == 0U))
    {
        (void)puts("测试总数=85，通过=85，失败=0");
    }

    return (s_test_failed == 0U) ? 0 : 1;
}
