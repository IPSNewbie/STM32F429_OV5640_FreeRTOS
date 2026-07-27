#ifndef IMAGE_REQUEST_PROTOCOL_H
#define IMAGE_REQUEST_PROTOCOL_H

#include <stdint.h>

//============================================================================
// @file    image_request_protocol.h
// @brief   图像请求协议解析模块（上位机下行命令）
// @note    定义帧格式：SOF(0xA5 0x5A) + 版本(1B) + 类型(1B) + 序列号(2B LE)
//          + 载荷长度(2B LE) + CRC32(4B LE) + EOF(0x0D 0x0A)
//          支持状态机解析、超时检测，适用于 UART 流式接收。
//============================================================================


//============================================================================
// 协议固定常量
//============================================================================

// 帧头同步字节 0（固定 0xA5）
#define IMAGE_REQUEST_SOF0              0xA5U

// 帧头同步字节 1（固定 0x5A）
#define IMAGE_REQUEST_SOF1              0x5AU

// 当前协议版本号
#define IMAGE_REQUEST_VERSION           0x01U

// 消息类型：请求图像（0x20）
#define IMAGE_REQUEST_MSG_REQUEST_IMAGE 0x20U

// 帧尾字节 0（回车 CR）
#define IMAGE_REQUEST_EOF0              0x0DU

// 帧尾字节 1（换行 LF）
#define IMAGE_REQUEST_EOF1              0x0AU

// 帧固定长度（不含有效载荷）：SOF2 + Version + Type + Seq2 + Len2 + CRC4 + EOF2 = 14 字节
#define IMAGE_REQUEST_FRAME_SIZE        14U

// V1 版本中载荷长度为 0（保留扩展）
#define IMAGE_REQUEST_PAYLOAD_LEN_V1    0U

// 解析超时阈值（毫秒）
#define IMAGE_REQUEST_TIMEOUT_MS        100U

//============================================================================
// 结构体：解析完成的请求帧信息
//============================================================================

/**
 * @brief  已解析的请求帧内容
 * @note   不含帧头/尾/CRC，仅包含业务字段
 */
typedef struct
{
    uint8_t version;      /**< 协议版本（应等于 IMAGE_REQUEST_VERSION） */
    uint8_t msg_type;     /**< 消息类型（应等于 IMAGE_REQUEST_MSG_REQUEST_IMAGE） */
    uint16_t seq;         /**< 序列号（小端，由上位机填充） */
    uint16_t payload_len; /**< 载荷长度（V1 固定为 0） */
} ImageRequestFrame_t;

//============================================================================
// 枚举：解析器状态机状态
//============================================================================

/**
 * @brief  解析器当前所处的状态
 * @note   依次接收 SOF0, SOF1, VERSION, TYPE, SEQ_L, SEQ_H, LEN_L, LEN_H,
 *         CRC0~CRC3, EOF0, EOF1
 */
typedef enum
{
    IMAGE_REQUEST_STATE_SYNC0 = 0,      /**< 等待 SOF0 (0xA5) */
    IMAGE_REQUEST_STATE_SYNC1,          /**< 等待 SOF1 (0x5A) */
    IMAGE_REQUEST_STATE_VERSION,        /**< 等待 Version 字段 */
    IMAGE_REQUEST_STATE_MSG_TYPE,       /**< 等待 MsgType 字段 */
    IMAGE_REQUEST_STATE_SEQ_LOW,        /**< 等待序列号低字节 */
    IMAGE_REQUEST_STATE_SEQ_HIGH,       /**< 等待序列号高字节 */
    IMAGE_REQUEST_STATE_LEN_LOW,        /**< 等待载荷长度低字节 */
    IMAGE_REQUEST_STATE_LEN_HIGH,       /**< 等待载荷长度高字节 */
    IMAGE_REQUEST_STATE_CRC0,           /**< 等待 CRC 字节 0 */
    IMAGE_REQUEST_STATE_CRC1,           /**< 等待 CRC 字节 1 */
    IMAGE_REQUEST_STATE_CRC2,           /**< 等待 CRC 字节 2 */
    IMAGE_REQUEST_STATE_CRC3,           /**< 等待 CRC 字节 3 */
    IMAGE_REQUEST_STATE_EOF0,           /**< 等待 EOF0 (0x0D) */
    IMAGE_REQUEST_STATE_EOF1            /**< 等待 EOF1 (0x0A) */
} ImageRequestParserState_t;

//============================================================================
// 枚举：解析结果码
//============================================================================

/**
 * @brief  单字节解析或超时检测的返回结果
 */
typedef enum
{
    IMAGE_REQUEST_PARSE_NONE = 0,           /**< 无有效结果（仍在处理） */
    IMAGE_REQUEST_PARSE_PENDING,            /**< 正在等待更多数据 */
    IMAGE_REQUEST_PARSE_OK,                 /**< 完整帧解析成功 */
    IMAGE_REQUEST_PARSE_CRC_ERROR,          /**< CRC 校验失败 */
    IMAGE_REQUEST_PARSE_VERSION_ERROR,      /**< 版本号不匹配 */
    IMAGE_REQUEST_PARSE_TYPE_ERROR,         /**< 消息类型不匹配（非请求图像） */
    IMAGE_REQUEST_PARSE_LENGTH_ERROR,       /**< 载荷长度不合法（非 0） */
    IMAGE_REQUEST_PARSE_EOF_ERROR,          /**< 帧尾错误（非 0x0D 0x0A） */
    IMAGE_REQUEST_PARSE_TIMEOUT,            /**< 接收超时（帧未完整接收） */
    IMAGE_REQUEST_PARSE_BAD_ARGUMENT        /**< 参数无效（空指针等） */
} ImageRequestParseResult_t;

//============================================================================
// 结构体：解析器上下文
//============================================================================

/**
 * @brief  解析器运行时上下文（需由调用者静态分配并维护）
 * @note   内部保存当前状态、已接收字段、CRC 累加值、时间戳等
 */
typedef struct
{
    ImageRequestParserState_t state;  /**< 当前状态机状态 */
    ImageRequestFrame_t frame;        /**< 正在接收的帧字段（版本/类型/序号/长度） */
    uint32_t computed_crc;            /**< 实时计算的有效 CRC（不含帧头/尾） */
    uint32_t received_crc;            /**< 从帧中解析出的接收端 CRC */
    uint32_t last_byte_time_ms;       /**< 最后一个字节接收的时间戳（用于超时） */
    uint8_t frame_active;             /**< 是否正在接收一帧（1=活跃） */
} ImageRequestParser_t;

//============================================================================
// 解析器接口函数
//============================================================================

/**
 * @brief  初始化解析器并进入空闲同步状态
 * @param  parser 由调用者静态分配的解析器指针
 * @note   将状态设为 IMAGE_REQUEST_STATE_SYNC0，并重置所有内部字段
 */
void ImageRequestProtocol_Init(ImageRequestParser_t *parser);

/**
 * @brief  清除候选帧并恢复空闲同步状态
 * @param  parser 由调用者静态分配的解析器指针
 * @note   与 Init 效果相同，用于丢弃当前正在接收的帧
 */
void ImageRequestProtocol_Reset(ImageRequestParser_t *parser);

/**
 * @brief  向解析器输入一个字节
 * @param  parser    解析器指针
 * @param  byte      当前输入的字节
 * @param  now_ms    调用者提供的当前毫秒时间戳
 * @param  out_frame 输出参数：仅在返回 IMAGE_REQUEST_PARSE_OK 时写入完整帧内容
 * @return 解析结果码（见 @ref ImageRequestParseResult_t）
 * @note   out_frame 必须是独立对象，不能指向 parser 内部的 frame 字段
 */
ImageRequestParseResult_t ImageRequestProtocol_FeedByte(
    ImageRequestParser_t *parser,
    uint8_t byte,
    uint32_t now_ms,
    ImageRequestFrame_t *out_frame);

/**
 * @brief  检查候选帧是否达到半帧超时
 * @param  parser 解析器指针
 * @param  now_ms 调用者提供的当前毫秒时间戳
 * @retval IMAGE_REQUEST_PARSE_TIMEOUT  帧接收超时（但未完成）
 * @retval IMAGE_REQUEST_PARSE_PENDING  帧仍在接收中（未超时）
 * @retval IMAGE_REQUEST_PARSE_NONE     空闲状态（无活跃帧）
 * @retval IMAGE_REQUEST_PARSE_BAD_ARGUMENT  参数无效
 * @note   调用者应定期（如每 50ms）调用此函数，以便及时清理超时帧
 */
ImageRequestParseResult_t ImageRequestProtocol_CheckTimeout(
    ImageRequestParser_t *parser,
    uint32_t now_ms);

/**
 * @brief  查询解析器是否正在接收候选帧
 * @param  parser 解析器指针
 * @retval 1 正在接收中（frame_active 为真）
 * @retval 0 空闲状态
 */
uint8_t ImageRequestProtocol_IsActive(const ImageRequestParser_t *parser);

#endif /* IMAGE_REQUEST_PROTOCOL_H */