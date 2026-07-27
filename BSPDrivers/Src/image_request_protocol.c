#include "image_request_protocol.h"
#include "protocol_crc32.h"
#include <stddef.h>

//============================================================================
// @file    image_request_protocol.c
// @brief   图像请求协议解析器实现
//          解析上位机发送的固定格式命令帧，包含 SOF、版本、类型、序列号、
//          长度、CRC32 和 EOF。采用状态机逐字节处理，支持超时检测。
//============================================================================

// 重置解析器到空闲同步状态（丢弃当前帧）
void ImageRequestProtocol_Reset(ImageRequestParser_t *parser)
{
    if (parser == NULL)
    {
        return;  // 无效指针直接返回
    }

    // 复位状态机到等待 SOF0
    parser->state = IMAGE_REQUEST_STATE_SYNC0;
    // 清空帧字段
    parser->frame.version = 0U;
    parser->frame.msg_type = 0U;
    parser->frame.seq = 0U;
    parser->frame.payload_len = 0U;
    // 重置 CRC 相关
    parser->computed_crc = 0U;
    parser->received_crc = 0U;
    // 清除时间戳和活跃标志
    parser->last_byte_time_ms = 0U;
    parser->frame_active = 0U;
}

// 初始化解析器（内部调用重置）
void ImageRequestProtocol_Init(ImageRequestParser_t *parser)
{
    ImageRequestProtocol_Reset(parser);
}

// 内部辅助：清除旧候选帧，并在当前字节为 A5 时保留新的帧头起点
static void ImageRequestProtocol_Resync(ImageRequestParser_t *parser,
                                        uint8_t current_byte,
                                        uint32_t now_ms)
{
    // 先完全重置
    ImageRequestProtocol_Reset(parser);

    // 如果当前字节是 SOF0，则直接进入等待 SOF1 状态，启动新帧
    if (current_byte == IMAGE_REQUEST_SOF0)
    {
        parser->state = IMAGE_REQUEST_STATE_SYNC1;
        parser->frame_active = 1U;          // 标记帧活跃
        parser->last_byte_time_ms = now_ms; // 记录时间戳
    }
}

// 内部辅助：接受当前字节并进入下一个状态，同时更新活跃标志和时间戳
static void ImageRequestProtocol_AcceptByte(ImageRequestParser_t *parser,
                                            ImageRequestParserState_t next_state,
                                            uint32_t now_ms)
{
    parser->state = next_state;
    parser->frame_active = 1U;
    parser->last_byte_time_ms = now_ms;
}

// 检查当前接收帧是否超时（半帧超时）
ImageRequestParseResult_t ImageRequestProtocol_CheckTimeout(
    ImageRequestParser_t *parser,
    uint32_t now_ms)
{
    if (parser == NULL)
    {
        return IMAGE_REQUEST_PARSE_BAD_ARGUMENT;
    }

    // 无活跃帧则返回空闲
    if (parser->frame_active == 0U)
    {
        return IMAGE_REQUEST_PARSE_NONE;
    }

    // 计算距离最后一次接收字节的时间差（无符号差值，可处理回绕）
    if ((uint32_t)(now_ms - parser->last_byte_time_ms) >=
        IMAGE_REQUEST_TIMEOUT_MS)
    {
        // 超时：重置解析器并返回超时错误
        ImageRequestProtocol_Reset(parser);
        return IMAGE_REQUEST_PARSE_TIMEOUT;
    }

    // 未超时，仍在等待更多数据
    return IMAGE_REQUEST_PARSE_PENDING;
}

// 查询解析器是否正在接收候选帧
uint8_t ImageRequestProtocol_IsActive(const ImageRequestParser_t *parser)
{
    if (parser == NULL)
    {
        return 0U;
    }
    return (parser->frame_active != 0U) ? 1U : 0U;
}

// 向解析器输入一个字节，驱动状态机
ImageRequestParseResult_t ImageRequestProtocol_FeedByte(
    ImageRequestParser_t *parser,
    uint8_t byte,
    uint32_t now_ms,
    ImageRequestFrame_t *out_frame)
{
    uint32_t final_crc;

    // 参数校验
    if ((parser == NULL) || (out_frame == NULL))
    {
        return IMAGE_REQUEST_PARSE_BAD_ARGUMENT;
    }

    // 新字节到达前，先检查当前帧是否已超时，若超时则重置并尝试重新同步
    if ((parser->frame_active != 0U) &&
        ((uint32_t)(now_ms - parser->last_byte_time_ms) >=
         IMAGE_REQUEST_TIMEOUT_MS))
    {
        ImageRequestProtocol_Resync(parser, byte, now_ms);
        return IMAGE_REQUEST_PARSE_TIMEOUT;
    }

    // 根据当前状态处理字节
    switch (parser->state)
    {
        // 等待 SOF0 (0xA5)
        case IMAGE_REQUEST_STATE_SYNC0:
            if (byte == IMAGE_REQUEST_SOF0)
            {
                // 找到 SOF0，进入 SYNC1 状态，开始新帧
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_PENDING;
            }
            // 非 SOF0 则忽略
            return IMAGE_REQUEST_PARSE_NONE;

        // 等待 SOF1 (0x5A)
        case IMAGE_REQUEST_STATE_SYNC1:
            if (byte == IMAGE_REQUEST_SOF1)
            {
                // 帧头匹配，初始化 CRC 计算
                parser->computed_crc = Protocol_CRC32_Init();
                // 转到下一个状态（版本字段）
                ImageRequestProtocol_AcceptByte(parser,
                                                IMAGE_REQUEST_STATE_VERSION,
                                                now_ms);
                return IMAGE_REQUEST_PARSE_PENDING;
            }
            // 如果当前字节又是 SOF0，则重新同步（可能上一个 SOF1 是噪声）
            if (byte == IMAGE_REQUEST_SOF0)
            {
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_PENDING;
            }
            // 否则丢弃并重新同步
            ImageRequestProtocol_Resync(parser, byte, now_ms);
            return IMAGE_REQUEST_PARSE_NONE;

        // 等待版本号
        case IMAGE_REQUEST_STATE_VERSION:
            if (byte != IMAGE_REQUEST_VERSION)
            {
                // 版本不匹配，重新同步并返回错误
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_VERSION_ERROR;
            }
            // 保存版本，更新 CRC
            parser->frame.version = byte;
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            // 转到下一状态
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_MSG_TYPE,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待消息类型
        case IMAGE_REQUEST_STATE_MSG_TYPE:
            if (byte != IMAGE_REQUEST_MSG_REQUEST_IMAGE)
            {
                // 类型错误，重新同步
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_TYPE_ERROR;
            }
            parser->frame.msg_type = byte;
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_SEQ_LOW,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待序列号低字节
        case IMAGE_REQUEST_STATE_SEQ_LOW:
            parser->frame.seq = (uint16_t)byte;  // 先存低字节
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_SEQ_HIGH,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待序列号高字节
        case IMAGE_REQUEST_STATE_SEQ_HIGH:
            parser->frame.seq |= (uint16_t)((uint16_t)byte << 8U); // 组合成 16 位
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_LEN_LOW,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待载荷长度低字节
        case IMAGE_REQUEST_STATE_LEN_LOW:
            parser->frame.payload_len = (uint16_t)byte;
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_LEN_HIGH,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待载荷长度高字节
        case IMAGE_REQUEST_STATE_LEN_HIGH:
            parser->frame.payload_len |=
                (uint16_t)((uint16_t)byte << 8U);
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            // V1 版本载荷长度必须为 0，否则错误
            if (parser->frame.payload_len != IMAGE_REQUEST_PAYLOAD_LEN_V1)
            {
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_LENGTH_ERROR;
            }
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC0,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 接收 CRC 字节 0
        case IMAGE_REQUEST_STATE_CRC0:
            parser->received_crc = (uint32_t)byte;  // 存低字节
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC1,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 接收 CRC 字节 1
        case IMAGE_REQUEST_STATE_CRC1:
            parser->received_crc |= (uint32_t)byte << 8U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC2,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 接收 CRC 字节 2
        case IMAGE_REQUEST_STATE_CRC2:
            parser->received_crc |= (uint32_t)byte << 16U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC3,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 接收 CRC 字节 3
        case IMAGE_REQUEST_STATE_CRC3:
            parser->received_crc |= (uint32_t)byte << 24U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_EOF0,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待 EOF0 (0x0D)
        case IMAGE_REQUEST_STATE_EOF0:
            if (byte != IMAGE_REQUEST_EOF0)
            {
                // EOF0 错误，重新同步
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_EOF_ERROR;
            }
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_EOF1,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // 等待 EOF1 (0x0A)
        case IMAGE_REQUEST_STATE_EOF1:
            if (byte != IMAGE_REQUEST_EOF1)
            {
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_EOF_ERROR;
            }

            // 帧尾正确，完成 CRC 计算并校验
            final_crc = Protocol_CRC32_Finalize(parser->computed_crc);
            if (final_crc != parser->received_crc)
            {
                // CRC 不匹配，错误
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_CRC_ERROR;
            }

            // 解析成功：将帧内容复制到输出结构体
            *out_frame = parser->frame;
            // 重置解析器，准备下一帧
            ImageRequestProtocol_Reset(parser);
            return IMAGE_REQUEST_PARSE_OK;

        // 未期望的状态（防御）
        default:
            ImageRequestProtocol_Resync(parser, byte, now_ms);
            return IMAGE_REQUEST_PARSE_NONE;
    }
}