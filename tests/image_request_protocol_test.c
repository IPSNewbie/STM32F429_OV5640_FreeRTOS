/*
 * 二进制图像请求协议主机侧单元测试。
 *
 * 覆盖固定向量、CRC32、状态机同步、字段错误、超时、自动复位和输出参数保护。
 */
#include "image_request_protocol.h"
#include "protocol_crc32.h"

#include <stdio.h>
#include <string.h>

#define FIXED_REQUEST_CRC 0xDBB445EAU
#define EXPECTED_TEST_COUNT 47U

static uint32_t s_test_total;
static uint32_t s_test_passed;
static uint32_t s_test_failed;

static const uint8_t s_fixed_request[IMAGE_REQUEST_FRAME_SIZE] = {
    0xA5U, 0x5AU, 0x01U, 0x20U, 0x34U, 0x12U, 0x00U,
    0x00U, 0xEAU, 0x45U, 0xB4U, 0xDBU, 0x0DU, 0x0AU
};

// 构造指定序号的合法图像请求帧并返回其 CRC32
static uint32_t BuildValidRequestFrame(
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

    // CRC 只覆盖版本、消息类型、序号和载荷长度共六个字节
    crc = Protocol_CRC32_Calculate(&frame[2], 6U);
    frame[8] = (uint8_t)(crc & 0xFFU);
    frame[9] = (uint8_t)((crc >> 8U) & 0xFFU);
    frame[10] = (uint8_t)((crc >> 16U) & 0xFFU);
    frame[11] = (uint8_t)((crc >> 24U) & 0xFFU);
    frame[12] = IMAGE_REQUEST_EOF0;
    frame[13] = IMAGE_REQUEST_EOF1;

    return crc;
}

// 用哨兵值填充输出帧，便于检测非成功路径的意外写入
static void SetFrameSentinel(ImageRequestFrame_t *frame)
{
    frame->version = 0xA7U;
    frame->msg_type = 0xB8U;
    frame->seq = 0xC9DAU;
    frame->payload_len = 0xEBFCU;
}

// 检查输出帧是否仍保持哨兵值
static int FrameIsSentinel(const ImageRequestFrame_t *frame)
{
    return (frame->version == 0xA7U) &&
           (frame->msg_type == 0xB8U) &&
           (frame->seq == 0xC9DAU) &&
           (frame->payload_len == 0xEBFCU);
}

// 检查解析结果是否匹配固定协议字段和预期序号
static int FrameMatches(const ImageRequestFrame_t *frame, uint16_t seq)
{
    return (frame->version == IMAGE_REQUEST_VERSION) &&
           (frame->msg_type == IMAGE_REQUEST_MSG_REQUEST_IMAGE) &&
           (frame->seq == seq) &&
           (frame->payload_len == IMAGE_REQUEST_PAYLOAD_LEN_V1);
}

// 比较两个解析器上下文的全部运行字段
static int ParserMatches(const ImageRequestParser_t *left,
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

// 检查解析器是否已恢复初始清零状态
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

// 检查解析器是否仅保留最新帧头起始字节
static int ParserHasFreshSof(const ImageRequestParser_t *parser,
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

// 连续输入一段字节并返回最后一次解析结果
static ImageRequestParseResult_t FeedData(
    ImageRequestParser_t *parser,
    const uint8_t *data,
    size_t length,
    uint32_t start_time_ms,
    uint32_t step_ms,
    ImageRequestFrame_t *out_frame,
    uint32_t *ok_count)
{
    ImageRequestParseResult_t result = IMAGE_REQUEST_PARSE_NONE;
    size_t i;

    for (i = 0U; i < length; ++i)
    {
        result = ImageRequestProtocol_FeedByte(
            parser,
            data[i],
            start_time_ms + ((uint32_t)i * step_ms),
            out_frame);
        if ((result == IMAGE_REQUEST_PARSE_OK) && (ok_count != NULL))
        {
            (*ok_count)++;
        }
    }

    return result;
}

// 验证指定序号的合法帧能够完整解析
static int ParseValidSeq(uint16_t seq)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    ImageRequestParseResult_t result;

    (void)BuildValidRequestFrame(seq, frame);
    SetFrameSentinel(&out_frame);
    ImageRequestProtocol_Init(&parser);
    result = FeedData(&parser,
                      frame,
                      sizeof(frame),
                      0U,
                      1U,
                      &out_frame,
                      NULL);
    return (result == IMAGE_REQUEST_PARSE_OK) &&
           FrameMatches(&out_frame, seq) &&
           ParserIsCleared(&parser);
}

// 验证固定请求向量、CRC 和业务字段
static int Test01FixedFrame(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t crc;
    ImageRequestParseResult_t result;

    crc = BuildValidRequestFrame(0x1234U, frame);
    if ((crc != FIXED_REQUEST_CRC) ||
        (memcmp(frame, s_fixed_request, sizeof(frame)) != 0))
    {
        return 0;
    }

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    result = FeedData(&parser, frame, sizeof(frame), 0U, 1U, &out_frame, NULL);
    return (result == IMAGE_REQUEST_PARSE_OK) &&
           FrameMatches(&out_frame, 0x1234U);
}

// 验证逐字节输入时各中间状态均保持等待
static int Test04SingleByteFeed(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    ImageRequestParseResult_t result;
    size_t i;

    (void)BuildValidRequestFrame(0x2345U, frame);
    SetFrameSentinel(&out_frame);
    ImageRequestProtocol_Init(&parser);

    for (i = 0U; i < (sizeof(frame) - 1U); ++i)
    {
        result = ImageRequestProtocol_FeedByte(
            &parser, frame[i], (uint32_t)i, &out_frame);
        if ((result != IMAGE_REQUEST_PARSE_PENDING) ||
            !FrameIsSentinel(&out_frame))
        {
            return 0;
        }
    }

    result = ImageRequestProtocol_FeedByte(
        &parser, frame[sizeof(frame) - 1U], (uint32_t)(sizeof(frame) - 1U),
        &out_frame);
    return (result == IMAGE_REQUEST_PARSE_OK) &&
           FrameMatches(&out_frame, 0x2345U);
}

// 验证循环输入完整帧后只在末字节成功
static int Test05FullLoop(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t ok_count = 0U;
    ImageRequestParseResult_t result;

    (void)BuildValidRequestFrame(0x3456U, frame);
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    result = FeedData(&parser, frame, sizeof(frame), 10U, 1U,
                      &out_frame, &ok_count);
    return (result == IMAGE_REQUEST_PARSE_OK) && (ok_count == 1U) &&
           FrameMatches(&out_frame, 0x3456U);
}

// 验证连续两帧可独立解析
static int Test06TwoFrames(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t first[IMAGE_REQUEST_FRAME_SIZE];
    uint8_t second[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t ok_count = 0U;

    (void)BuildValidRequestFrame(0x1001U, first);
    (void)BuildValidRequestFrame(0x1002U, second);
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    (void)FeedData(&parser, first, sizeof(first), 0U, 1U,
                   &out_frame, &ok_count);
    if ((ok_count != 1U) || !FrameMatches(&out_frame, 0x1001U))
    {
        return 0;
    }
    (void)FeedData(&parser, second, sizeof(second), 14U, 1U,
                   &out_frame, &ok_count);
    return (ok_count == 2U) && FrameMatches(&out_frame, 0x1002U);
}

// 验证连续十帧序号递增解析的稳定性
static int Test07TenFrames(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t ok_count = 0U;
    uint32_t i;

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    for (i = 0U; i < 10U; ++i)
    {
        (void)BuildValidRequestFrame((uint16_t)i, frame);
        if (FeedData(&parser, frame, sizeof(frame), i * 20U, 1U,
                     &out_frame, &ok_count) != IMAGE_REQUEST_PARSE_OK)
        {
            return 0;
        }
        if (!FrameMatches(&out_frame, (uint16_t)i))
        {
            return 0;
        }
    }
    return (ok_count == 10U) && FrameMatches(&out_frame, 9U);
}

// 验证终态结果后解析器自动复位
static int Test08AutomaticReset(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x2001U, frame);
    ImageRequestProtocol_Init(&parser);
    if ((FeedData(&parser, frame, sizeof(frame), 0U, 1U,
                  &out_frame, NULL) != IMAGE_REQUEST_PARSE_OK) ||
        !ParserIsCleared(&parser))
    {
        return 0;
    }

    (void)BuildValidRequestFrame(0x2002U, frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, frame[0], 20U, &out_frame) !=
         IMAGE_REQUEST_PARSE_PENDING) ||
        !ParserHasFreshSof(&parser, 20U))
    {
        return 0;
    }
    return (FeedData(&parser, &frame[1], sizeof(frame) - 1U, 21U, 1U,
                     &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK) &&
           FrameMatches(&out_frame, 0x2002U);
}

// 验证随机前缀后仍可同步合法帧
static int Test09GarbagePrefix(void)
{
    static const uint8_t garbage[] = {0x00U, 0xFFU, 0x5AU, 0x20U, 0x0DU};
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    size_t i;

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    for (i = 0U; i < sizeof(garbage); ++i)
    {
        if (ImageRequestProtocol_FeedByte(
                &parser, garbage[i], (uint32_t)i, &out_frame) !=
            IMAGE_REQUEST_PARSE_NONE)
        {
            return 0;
        }
    }

    (void)BuildValidRequestFrame(0x3001U, frame);
    return FeedData(&parser, frame, sizeof(frame), 10U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证重复帧头首字节后的重新同步
static int Test10A5A55A(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x3002U, frame);
    ImageRequestProtocol_Init(&parser);
    if (ImageRequestProtocol_FeedByte(
            &parser, IMAGE_REQUEST_SOF0, 0U, &out_frame) !=
        IMAGE_REQUEST_PARSE_PENDING)
    {
        return 0;
    }
    return FeedData(&parser, frame, sizeof(frame), 1U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证损坏帧头不会进入活跃帧状态
static int Test11BrokenPrefix(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    ImageRequestProtocol_Init(&parser);
    if ((ImageRequestProtocol_FeedByte(
             &parser, IMAGE_REQUEST_SOF0, 0U, &out_frame) !=
         IMAGE_REQUEST_PARSE_PENDING) ||
        (ImageRequestProtocol_FeedByte(
             &parser, 0x00U, 1U, &out_frame) != IMAGE_REQUEST_PARSE_NONE))
    {
        return 0;
    }
    (void)BuildValidRequestFrame(0x3003U, frame);
    return FeedData(&parser, frame, sizeof(frame), 2U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证多个连续 0xA5 始终保留最新起点
static int Test12RepeatedA5(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t i;

    (void)BuildValidRequestFrame(0x3004U, frame);
    ImageRequestProtocol_Init(&parser);
    for (i = 0U; i < 4U; ++i)
    {
        if (ImageRequestProtocol_FeedByte(
                &parser, IMAGE_REQUEST_SOF0, i, &out_frame) !=
            IMAGE_REQUEST_PARSE_PENDING)
        {
            return 0;
        }
    }
    return FeedData(&parser, &frame[1], sizeof(frame) - 1U, 4U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证错误帧之后可以解析下一合法帧
static int Test13BadThenValid(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t bad[IMAGE_REQUEST_FRAME_SIZE];
    uint8_t good[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t error_count = 0U;
    size_t i;

    (void)BuildValidRequestFrame(0x3005U, bad);
    bad[8] ^= 0x01U;
    (void)BuildValidRequestFrame(0x3006U, good);
    ImageRequestProtocol_Init(&parser);
    for (i = 0U; i < sizeof(bad); ++i)
    {
        if (ImageRequestProtocol_FeedByte(
                &parser, bad[i], (uint32_t)i, &out_frame) ==
            IMAGE_REQUEST_PARSE_CRC_ERROR)
        {
            error_count++;
        }
    }
    return (error_count == 1U) &&
           (FeedData(&parser, good, sizeof(good), 20U, 1U,
                     &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK);
}

// 验证错误字段值为 0xA5 时作为下一候选帧起点
static int Test14ErrorByteA5(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x3007U, frame);
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 0U, &out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF1, 1U, &out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, IMAGE_REQUEST_SOF0, 2U, &out_frame) !=
         IMAGE_REQUEST_PARSE_VERSION_ERROR) ||
        !ParserHasFreshSof(&parser, 2U) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }
    return FeedData(&parser, &frame[1], sizeof(frame) - 1U, 3U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证指定字段损坏时返回预期错误且不写输出
static int TestFieldError(size_t index,
                          uint8_t value,
                          size_t feed_length,
                          ImageRequestParseResult_t expected)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    ImageRequestParseResult_t result;

    (void)BuildValidRequestFrame(0x4001U, frame);
    frame[index] = value;
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    result = FeedData(&parser, frame, feed_length, 0U, 1U,
                      &out_frame, NULL);
    return (result == expected) && FrameIsSentinel(&out_frame);
}

// 验证指定字段错误后的自动恢复
static int TestErrorRecovery(size_t index,
                             uint8_t value,
                             ImageRequestParseResult_t expected)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t bad[IMAGE_REQUEST_FRAME_SIZE];
    uint8_t good[IMAGE_REQUEST_FRAME_SIZE];
    ImageRequestParseResult_t result;
    uint32_t expected_error_count = 0U;
    uint32_t unexpected_result_count = 0U;
    uint32_t ok_count = 0U;
    size_t i;

    (void)BuildValidRequestFrame(0x4002U, bad);
    bad[index] = value;
    (void)BuildValidRequestFrame(0x4003U, good);
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    for (i = 0U; i < sizeof(bad); ++i)
    {
        result = ImageRequestProtocol_FeedByte(
            &parser, bad[i], (uint32_t)i, &out_frame);
        if (result == expected)
        {
            expected_error_count++;
        }
        else if ((result != IMAGE_REQUEST_PARSE_NONE) &&
                 (result != IMAGE_REQUEST_PARSE_PENDING))
        {
            unexpected_result_count++;
        }
    }

    if ((expected_error_count != 1U) ||
        (unexpected_result_count != 0U) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }

    return (FeedData(&parser, good, sizeof(good), 14U, 1U,
                     &out_frame, &ok_count) == IMAGE_REQUEST_PARSE_OK) &&
           (ok_count == 1U) &&
           FrameMatches(&out_frame, 0x4003U);
}

// 验证帧尾错误优先于成功结果报告
static int Test20EofPriority(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x4004U, frame);
    frame[8] ^= 0x01U;
    frame[12] = 0x00U;
    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    return (FeedData(&parser, frame, 13U, 0U, 1U,
                     &out_frame, NULL) == IMAGE_REQUEST_PARSE_EOF_ERROR) &&
           FrameIsSentinel(&out_frame);
}

// 验证字节间隔小于阈值时不超时
static int Test31ShortIntervals(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x5001U, frame);
    ImageRequestProtocol_Init(&parser);
    return FeedData(&parser, frame, sizeof(frame), 0U, 99U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证超时后普通字节被丢弃并恢复空闲
static int Test32TimedOutNonA5(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 1000U, &out_frame);
    return (ImageRequestProtocol_FeedByte(
                &parser, 0x00U, 1100U, &out_frame) ==
            IMAGE_REQUEST_PARSE_TIMEOUT) &&
           ParserIsCleared(&parser) && FrameIsSentinel(&out_frame);
}

// 验证超时后的 0xA5 同时作为新帧起点
static int Test33TimedOutA5(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 1000U, &out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, IMAGE_REQUEST_SOF0, 1100U, &out_frame) !=
         IMAGE_REQUEST_PARSE_TIMEOUT) ||
        !ParserHasFreshSof(&parser, 1100U) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }

    (void)BuildValidRequestFrame(0x5003U, frame);
    return (FeedData(&parser, &frame[1], sizeof(frame) - 1U, 1101U, 1U,
                     &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK) &&
           FrameMatches(&out_frame, 0x5003U);
}

// 验证半帧超时后下一合法帧仍可解析
static int Test34ValidAfterTimeout(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 0U, &out_frame);
    if (ImageRequestProtocol_CheckTimeout(&parser, 100U) !=
        IMAGE_REQUEST_PARSE_TIMEOUT)
    {
        return 0;
    }
    (void)BuildValidRequestFrame(0x5002U, frame);
    return FeedData(&parser, frame, sizeof(frame), 101U, 1U,
                    &out_frame, NULL) == IMAGE_REQUEST_PARSE_OK;
}

// 验证输出指针为空时解析器状态不被破坏
static int Test40NullOutputKeepsState(void)
{
    ImageRequestParser_t parser;
    ImageRequestParser_t before;
    ImageRequestFrame_t out_frame;

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 10U, &out_frame);
    before = parser;
    if ((ImageRequestProtocol_FeedByte(
             &parser, IMAGE_REQUEST_SOF1, 11U, NULL) !=
         IMAGE_REQUEST_PARSE_BAD_ARGUMENT) ||
        !ParserMatches(&parser, &before))
    {
        return 0;
    }

    return (ImageRequestProtocol_FeedByte(
                &parser, IMAGE_REQUEST_SOF1, 11U, &out_frame) ==
            IMAGE_REQUEST_PARSE_PENDING) &&
           (parser.state == IMAGE_REQUEST_STATE_VERSION);
}

// 验证非成功结果不会修改调用方输出帧
static int Test43NonOkKeepsOutput(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, 0x00U, 0U, &out_frame) != IMAGE_REQUEST_PARSE_NONE) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }
    if ((ImageRequestProtocol_FeedByte(
             &parser, IMAGE_REQUEST_SOF0, 1U, &out_frame) !=
         IMAGE_REQUEST_PARSE_PENDING) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF1, 2U, &out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, 0x02U, 3U, &out_frame) !=
         IMAGE_REQUEST_PARSE_VERSION_ERROR) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }

    (void)BuildValidRequestFrame(0x6002U, frame);
    if (!TestFieldError(3U, 0x21U, 4U, IMAGE_REQUEST_PARSE_TYPE_ERROR) ||
        !TestFieldError(6U, 0x01U, 8U, IMAGE_REQUEST_PARSE_LENGTH_ERROR) ||
        !TestFieldError(8U, (uint8_t)(frame[8] ^ 0x01U),
                        IMAGE_REQUEST_FRAME_SIZE,
                        IMAGE_REQUEST_PARSE_CRC_ERROR) ||
        !TestFieldError(12U, 0x00U, 13U,
                        IMAGE_REQUEST_PARSE_EOF_ERROR))
    {
        return 0;
    }

    ImageRequestProtocol_Init(&parser);
    SetFrameSentinel(&out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 10U, &out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, 0x00U, 110U, &out_frame) !=
         IMAGE_REQUEST_PARSE_TIMEOUT) ||
        !FrameIsSentinel(&out_frame))
    {
        return 0;
    }

    return (ImageRequestProtocol_FeedByte(
                NULL, 0x00U, 0U, &out_frame) ==
            IMAGE_REQUEST_PARSE_BAD_ARGUMENT) &&
           FrameIsSentinel(&out_frame);
}

// 验证仅在完整合法帧终态写入一次输出
static int Test44OutputWrittenOnce(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    ImageRequestFrame_t saved;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x6001U, frame);
    ImageRequestProtocol_Init(&parser);
    if (FeedData(&parser, frame, sizeof(frame), 0U, 1U,
                 &out_frame, NULL) != IMAGE_REQUEST_PARSE_OK)
    {
        return 0;
    }
    saved = out_frame;
    return (ImageRequestProtocol_FeedByte(
                &parser, 0x00U, 20U, &out_frame) ==
            IMAGE_REQUEST_PARSE_NONE) &&
           FrameMatches(&out_frame, saved.seq) &&
           (out_frame.version == saved.version) &&
           (out_frame.msg_type == saved.msg_type) &&
           (out_frame.payload_len == saved.payload_len);
}

// 验证显式复位清除全部内部字段
static int Test45ResetClearsFields(void)
{
    ImageRequestParser_t parser;

    parser.state = IMAGE_REQUEST_STATE_EOF1;
    parser.frame.version = 1U;
    parser.frame.msg_type = 2U;
    parser.frame.seq = 3U;
    parser.frame.payload_len = 4U;
    parser.computed_crc = 5U;
    parser.received_crc = 6U;
    parser.last_byte_time_ms = 7U;
    parser.frame_active = 1U;
    ImageRequestProtocol_Reset(&parser);
    if (!ParserIsCleared(&parser))
    {
        return 0;
    }

    parser.state = IMAGE_REQUEST_STATE_CRC3;
    parser.frame.version = 8U;
    parser.frame.msg_type = 9U;
    parser.frame.seq = 10U;
    parser.frame.payload_len = 11U;
    parser.computed_crc = 12U;
    parser.received_crc = 13U;
    parser.last_byte_time_ms = 14U;
    parser.frame_active = 1U;
    ImageRequestProtocol_Init(&parser);
    return ParserIsCleared(&parser);
}

// 验证候选帧接收期间活跃标志保持有效
static int Test46ActiveDuringParsing(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    ImageRequestParseResult_t result;
    size_t i;

    (void)BuildValidRequestFrame(0x6003U, frame);
    ImageRequestProtocol_Init(&parser);
    if (ImageRequestProtocol_IsActive(&parser) != 0U)
    {
        return 0;
    }
    for (i = 0U; i < (sizeof(frame) - 1U); ++i)
    {
        result = ImageRequestProtocol_FeedByte(
            &parser, frame[i], (uint32_t)i, &out_frame);
        if ((result != IMAGE_REQUEST_PARSE_PENDING) ||
            (ImageRequestProtocol_IsActive(&parser) != 1U))
        {
            return 0;
        }
    }
    return 1;
}

// 验证成功或错误终态后活跃标志清除
static int Test47InactiveAfterTerminalResults(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];

    (void)BuildValidRequestFrame(0x7001U, frame);
    ImageRequestProtocol_Init(&parser);
    if ((FeedData(&parser, frame, sizeof(frame), 0U, 1U,
                  &out_frame, NULL) != IMAGE_REQUEST_PARSE_OK) ||
        (ImageRequestProtocol_IsActive(&parser) != 0U))
    {
        return 0;
    }

    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 20U, &out_frame);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF1, 21U, &out_frame);
    if ((ImageRequestProtocol_FeedByte(
             &parser, 0x02U, 22U, &out_frame) !=
         IMAGE_REQUEST_PARSE_VERSION_ERROR) ||
        (ImageRequestProtocol_IsActive(&parser) != 0U))
    {
        return 0;
    }

    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 30U, &out_frame);
    return (ImageRequestProtocol_CheckTimeout(&parser, 130U) ==
            IMAGE_REQUEST_PARSE_TIMEOUT) &&
           (ImageRequestProtocol_IsActive(&parser) == 0U);
}

// 记录单项测试结果并输出失败名称
static void RunTest(const char *name, int passed)
{
    s_test_total++;
    if (passed != 0)
    {
        s_test_passed++;
        return;
    }

    s_test_failed++;
    (void)fprintf(stderr, "测试失败：%s\n", name);
}

// 运行图像请求协议全部主机侧单元测试
int main(void)
{
    ImageRequestParser_t parser;
    ImageRequestFrame_t out_frame;
    uint8_t frame[IMAGE_REQUEST_FRAME_SIZE];
    uint32_t fixed_crc;

    fixed_crc = BuildValidRequestFrame(0x1234U, frame);

    RunTest("01 固定合法帧", Test01FixedFrame());
    RunTest("02 seq=0x0000", ParseValidSeq(0x0000U));
    RunTest("03 seq=0xFFFF", ParseValidSeq(0xFFFFU));
    RunTest("04 逐字节输入", Test04SingleByteFeed());
    RunTest("05 循环输入完整帧", Test05FullLoop());
    RunTest("06 两个连续合法帧", Test06TwoFrames());
    RunTest("07 连续十个合法帧", Test07TenFrames());
    RunTest("08 成功后自动复位", Test08AutomaticReset());

    RunTest("09 随机垃圾前缀", Test09GarbagePrefix());
    RunTest("10 A5 A5 5A 重同步", Test10A5A55A());
    RunTest("11 A5 00 后恢复", Test11BrokenPrefix());
    RunTest("12 连续多个 A5", Test12RepeatedA5());
    RunTest("13 错误帧后合法帧", Test13BadThenValid());
    RunTest("14 错误字节 A5 保留", Test14ErrorByteA5());

    RunTest("15 版本错误",
            TestFieldError(2U, 0x02U, 3U,
                           IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("16 消息类型错误",
            TestFieldError(3U, 0x21U, 4U,
                           IMAGE_REQUEST_PARSE_TYPE_ERROR));
    RunTest("17 载荷长度为1",
            TestFieldError(6U, 0x01U, 8U,
                           IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("18 载荷长度为0x0100",
            TestFieldError(7U, 0x01U, 8U,
                           IMAGE_REQUEST_PARSE_LENGTH_ERROR));
    RunTest("19 CRC位翻转",
            TestFieldError(8U, (uint8_t)(frame[8] ^ 0x01U),
                           IMAGE_REQUEST_FRAME_SIZE,
                           IMAGE_REQUEST_PARSE_CRC_ERROR));
    RunTest("20 EOF0错误优先于CRC错误", Test20EofPriority());
    RunTest("21 EOF1错误",
            TestFieldError(13U, 0x00U, IMAGE_REQUEST_FRAME_SIZE,
                           IMAGE_REQUEST_PARSE_EOF_ERROR));
    RunTest("22 CRC错误后恢复",
            TestErrorRecovery(8U, (uint8_t)(frame[8] ^ 0x01U),
                              IMAGE_REQUEST_PARSE_CRC_ERROR));
    RunTest("23 EOF错误后恢复",
            TestErrorRecovery(12U, 0x00U,
                              IMAGE_REQUEST_PARSE_EOF_ERROR));
    RunTest("24 版本错误后恢复",
            TestErrorRecovery(2U, 0x02U,
                              IMAGE_REQUEST_PARSE_VERSION_ERROR));
    RunTest("25 类型错误后恢复",
            TestErrorRecovery(3U, 0x21U,
                              IMAGE_REQUEST_PARSE_TYPE_ERROR));
    RunTest("26 长度错误后恢复",
            TestErrorRecovery(6U, 0x01U,
                              IMAGE_REQUEST_PARSE_LENGTH_ERROR));

    ImageRequestProtocol_Init(&parser);
    RunTest("27 空闲状态不超时",
            ImageRequestProtocol_CheckTimeout(&parser, 100U) ==
                IMAGE_REQUEST_PARSE_NONE);

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 1000U, &out_frame);
    RunTest("28 经过99毫秒仍等待",
            (ImageRequestProtocol_CheckTimeout(&parser, 1099U) ==
             IMAGE_REQUEST_PARSE_PENDING) &&
            ParserHasFreshSof(&parser, 1000U));

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 1000U, &out_frame);
    RunTest("29 经过100毫秒超时",
            (ImageRequestProtocol_CheckTimeout(&parser, 1100U) ==
             IMAGE_REQUEST_PARSE_TIMEOUT) && ParserIsCleared(&parser));

    ImageRequestProtocol_Init(&parser);
    (void)FeedData(&parser, frame, 5U, 2000U, 1U, &out_frame, NULL);
    RunTest("30 半帧超时",
            (ImageRequestProtocol_CheckTimeout(&parser, 2104U) ==
             IMAGE_REQUEST_PARSE_TIMEOUT) && ParserIsCleared(&parser));

    RunTest("31 字节间隔小于超时", Test31ShortIntervals());
    RunTest("32 超时新字节不是A5", Test32TimedOutNonA5());
    RunTest("33 超时新字节是A5", Test33TimedOutA5());
    RunTest("34 超时后合法帧", Test34ValidAfterTimeout());

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 0xFFFFFFF0U, &out_frame);
    RunTest("35 tick回绕未达到100毫秒",
            (ImageRequestProtocol_CheckTimeout(
                 &parser, (uint32_t)(0xFFFFFFF0U + 99U)) ==
             IMAGE_REQUEST_PARSE_PENDING) &&
            ParserHasFreshSof(&parser, 0xFFFFFFF0U));

    ImageRequestProtocol_Init(&parser);
    (void)ImageRequestProtocol_FeedByte(
        &parser, IMAGE_REQUEST_SOF0, 0xFFFFFFF0U, &out_frame);
    RunTest("36 tick回绕达到100毫秒",
            (ImageRequestProtocol_CheckTimeout(
                 &parser, (uint32_t)(0xFFFFFFF0U + 100U)) ==
             IMAGE_REQUEST_PARSE_TIMEOUT) &&
            ParserIsCleared(&parser));

    ImageRequestProtocol_Init(NULL);
    RunTest("37 Init空指针", 1);
    ImageRequestProtocol_Reset(NULL);
    RunTest("38 Reset空指针", 1);
    RunTest("39 FeedByte空解析器",
            ImageRequestProtocol_FeedByte(
                NULL, 0U, 0U, &out_frame) ==
                IMAGE_REQUEST_PARSE_BAD_ARGUMENT);
    RunTest("40 FeedByte空输出", Test40NullOutputKeepsState());
    RunTest("41 CheckTimeout空指针",
            ImageRequestProtocol_CheckTimeout(NULL, 0U) ==
                IMAGE_REQUEST_PARSE_BAD_ARGUMENT);
    RunTest("42 IsActive空指针",
            ImageRequestProtocol_IsActive(NULL) == 0U);
    RunTest("43 非成功结果保持输出", Test43NonOkKeepsOutput());
    RunTest("44 成功只写一次输出", Test44OutputWrittenOnce());
    RunTest("45 Reset清空内部字段", Test45ResetClearsFields());
    RunTest("46 解析过程中活动", Test46ActiveDuringParsing());
    RunTest("47 终止结果后空闲", Test47InactiveAfterTerminalResults());

    if (s_test_total != EXPECTED_TEST_COUNT)
    {
        (void)fprintf(stderr,
                      "测试数量错误：实际=%lu，期望=%lu\n",
                      (unsigned long)s_test_total,
                      (unsigned long)EXPECTED_TEST_COUNT);
        return 1;
    }

    (void)printf(
        "固定帧 CRC32=0x%08lX，测试总数=%lu，通过=%lu，失败=%lu\n",
        (unsigned long)fixed_crc,
        (unsigned long)s_test_total,
        (unsigned long)s_test_passed,
        (unsigned long)s_test_failed);

    return (s_test_failed == 0U) ? 0 : 1;
}
