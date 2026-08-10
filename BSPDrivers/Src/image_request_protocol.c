#include "image_request_protocol.h" // 定义 14 字节请求帧、解析状态、结果码和解析器上下文
#include "protocol_crc32.h"         // 提供字段 CRC32 的初始化、逐字节更新和最终化接口
#include <stddef.h>                  // 提供 NULL，用于公共接口参数校验

//============================================================================
// @file    image_request_protocol.c
// @brief   上位机 binary image request 的逐字节状态机解析器
//
// 本模块由 camera_uart_dispatcher 在 CameraServiceTask 上下文调用；UART ISR
// 只负责搬运字节，不在中断中解析协议。本模块不访问 UART、HAL 或 FreeRTOS API，
// 每次 FeedByte() 只消费一个字节，跨调用状态保存在 ImageRequestParser_t 中。
//
// V1 请求固定为 14 字节：
// [0]      SOF0 = 0xA5
// [1]      SOF1 = 0x5A
// [2]      version = 0x01
// [3]      message type = 0x20
// [4..5]   sequence，小端序
// [6..7]   payload length，小端序，V1 固定为 0
// [8..11]  CRC32，小端序，只覆盖 [2..7] 六个业务字段字节
// [12]     EOF0 = 0x0D
// [13]     EOF1 = 0x0A
//
// parser 发现字段错误后立即重同步；dispatcher 负责隔离旧固定帧的剩余尾部，
// 防止这些二进制字节被错误地送入文本 CLI。
//============================================================================

// 丢弃当前候选帧并恢复到等待 0xA5 的空闲同步状态。
// 该操作只清 parser 内部字段，不修改调用方此前已经取得的独立 out_frame。
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

// 初始化由调用者持有的解析器上下文；当前初始化语义与完整 Reset 相同。
// parser 应在 CameraServiceTask 开始接收字节前初始化一次。
void ImageRequestProtocol_Init(ImageRequestParser_t *parser)
{
    ImageRequestProtocol_Reset(parser);
}

// 清除旧候选帧，并把当前 0xA5 同时保留为下一候选帧的起点。
// 错误或超时发生时，当前字节可能恰好是新帧 SOF0；保留它可以避免丢失同步机会。
static void ImageRequestProtocol_Resync(ImageRequestParser_t *parser,
                                        uint8_t current_byte,
                                        uint32_t now_ms)
{
    // 先完全重置
    ImageRequestProtocol_Reset(parser);

    // 当前字节本身是 SOF0 时直接等待 SOF1，不要求上位机为重同步额外再发一个 0xA5
    if (current_byte == IMAGE_REQUEST_SOF0)
    {
        parser->state = IMAGE_REQUEST_STATE_SYNC1;
        parser->frame_active = 1U;          // 标记帧活跃
        parser->last_byte_time_ms = now_ms; // 记录时间戳
    }
}

// 接受当前字段字节、推进到下一个状态，并刷新相邻字节超时的起算时间。
// 只有通过当前字段校验的字节才调用此 helper，因此时间戳代表最后一个有效候选帧字节。
static void ImageRequestProtocol_AcceptByte(ImageRequestParser_t *parser,
                                            ImageRequestParserState_t next_state,
                                            uint32_t now_ms)
{
    parser->state = next_state;
    parser->frame_active = 1U;
    parser->last_byte_time_ms = now_ms;
}

// 在没有新字节到达时检查活动候选帧是否发生 inter-byte timeout。
// 100 ms 限制的是距最后一个已接受字节的间隔，而不是整帧从头到尾的累计时间。
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

    // 无符号时间差在 HAL tick 回绕后仍能正确比较相对间隔
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

// 查询解析器是否已看到 SOF0 且仍在等待候选帧的后续字段。
// dispatcher 根据该状态决定继续保持 BINARY 模式还是恢复 IDLE。
uint8_t ImageRequestProtocol_IsActive(const ImageRequestParser_t *parser)
{
    if (parser == NULL)
    {
        return 0U;
    }
    return (parser->frame_active != 0U) ? 1U : 0U;
}

// 在 CameraServiceTask 上下文输入一个字节并推进固定 14 字节请求状态机。
// 本函数没有内部等待或扫描循环：每次只处理当前 byte，成功时复制业务字段后自动 Reset。
// out_frame 只有在返回 IMAGE_REQUEST_PARSE_OK 时有效，且不能与 parser->frame 使用同一地址。
ImageRequestParseResult_t ImageRequestProtocol_FeedByte(
    ImageRequestParser_t *parser,
    uint8_t byte,
    uint32_t now_ms,
    ImageRequestFrame_t *out_frame)
{
    uint32_t final_crc; // 收到完整 EOF 后由增量内部状态最终化得到的可比较 CRC32

    // 参数校验
    if ((parser == NULL) || (out_frame == NULL))
    {
        return IMAGE_REQUEST_PARSE_BAD_ARGUMENT;
    }

    // 新字节到达前先检查相邻字节间隔；即使超时，当前 byte 若为 0xA5 仍可成为新帧起点
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
                // 两字节 SOF 完整匹配后才开始业务字段 CRC；SOF 自身不在 CRC 覆盖范围内
                parser->computed_crc = Protocol_CRC32_Init();
                // 转到下一个状态（版本字段）
                ImageRequestProtocol_AcceptByte(parser,
                                                IMAGE_REQUEST_STATE_VERSION,
                                                now_ms);
                return IMAGE_REQUEST_PARSE_PENDING;
            }
            // 连续遇到新的 SOF0 时把它保留为新起点，避免噪声中的 A5 A5 5A 丢失后一候选帧
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
            // version 是 CRC 覆盖的第一个业务字节，保存后立即更新增量状态
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
            // message type 是 CRC 覆盖的第二个业务字节
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_SEQ_LOW,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // sequence 按小端序传输：先接收低字节，再接收高字节并组合为 16 位值
        case IMAGE_REQUEST_STATE_SEQ_LOW:
            parser->frame.seq = (uint16_t)byte;  // 先存低字节
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_SEQ_HIGH,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // sequence 的两个原始字节都按接收顺序进入 CRC，CRC 不直接计算主机端整数内存表示
        case IMAGE_REQUEST_STATE_SEQ_HIGH:
            parser->frame.seq |= (uint16_t)((uint16_t)byte << 8U); // 组合成 16 位
            parser->computed_crc =
                Protocol_CRC32_UpdateByte(parser->computed_crc, byte);
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_LEN_LOW,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // payload length 同样按小端序传输；V1 请求只表达“请求一帧”，帧内不携带 payload
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
            // V1 请求的业务字段到此结束，长度必须为 0；version/type/seq/length 共六字节均已进入 CRC
            if (parser->frame.payload_len != IMAGE_REQUEST_PAYLOAD_LEN_V1)
            {
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_LENGTH_ERROR;
            }
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC0,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // CRC32 字段按小端序传输；CRC 字段本身不再反馈到 computed_crc
        case IMAGE_REQUEST_STATE_CRC0:
            parser->received_crc = (uint32_t)byte;  // 存低字节
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC1,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // CRC 字节 1 写入结果的 bit[15:8]
        case IMAGE_REQUEST_STATE_CRC1:
            parser->received_crc |= (uint32_t)byte << 8U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC2,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // CRC 字节 2 写入结果的 bit[23:16]
        case IMAGE_REQUEST_STATE_CRC2:
            parser->received_crc |= (uint32_t)byte << 16U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_CRC3,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // CRC 字节 3 写入结果的 bit[31:24]，完成接收端 CRC32 重组
        case IMAGE_REQUEST_STATE_CRC3:
            parser->received_crc |= (uint32_t)byte << 24U;
            ImageRequestProtocol_AcceptByte(parser,
                                            IMAGE_REQUEST_STATE_EOF0,
                                            now_ms);
            return IMAGE_REQUEST_PARSE_PENDING;

        // EOF 用于确认固定帧边界，不属于 CRC 覆盖范围
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

            // 只有完整 EOF 正确后才最终化并比较 CRC，避免把截断帧误报为单纯 CRC 错误
            final_crc = Protocol_CRC32_Finalize(parser->computed_crc);
            if (final_crc != parser->received_crc)
            {
                // CRC 不匹配，错误
                ImageRequestProtocol_Resync(parser, byte, now_ms);
                return IMAGE_REQUEST_PARSE_CRC_ERROR;
            }

            // 先复制业务字段，再 Reset 内部 parser；因此 out_frame 必须是独立对象，不能与 parser->frame 别名
            *out_frame = parser->frame;
            // 重置解析器，准备下一帧
            ImageRequestProtocol_Reset(parser);
            return IMAGE_REQUEST_PARSE_OK;

        // 正常状态机不会进入这里；若上下文被写入非法状态，丢弃候选帧并重新寻找 SOF0
        default:
            ImageRequestProtocol_Resync(parser, byte, now_ms);
            return IMAGE_REQUEST_PARSE_NONE;
    }
}
