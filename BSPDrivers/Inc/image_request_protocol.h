#ifndef IMAGE_REQUEST_PROTOCOL_H
#define IMAGE_REQUEST_PROTOCOL_H

#include <stdint.h>  // 提供协议字段和毫秒时间戳使用的固定宽度整数类型

/**
 * @file image_request_protocol.h
 * @brief 上位机 binary image request 的逐字节解析接口
 *
 * V1 请求固定为 14 字节：
 * - [0..1]：SOF0=0xA5、SOF1=0x5A，用于从混合 UART 字节流中定位候选帧；
 * - [2]：version，[3]：message type；
 * - [4..5]：sequence，小端；[6..7]：payload length，小端且 V1 固定为 0；
 * - [8..11]：CRC32，小端，只覆盖 [2..7] 六个业务字段字节；
 * - [12..13]：EOF0=CR、EOF1=LF。
 *
 * 解析器不访问 UART、HAL 或 FreeRTOS。CameraServiceTask 从 StreamBuffer 取字节后
 * 按接收顺序调用 FeedByte()；上下文必须跨调用保存，且不能由多个任务或 ISR 并发喂入。
 * 100 ms timeout 是相邻已接受字节之间的间隔，不是整帧累计时间。
 * CRC32 用于发现传输损坏，不提供身份认证或防篡改能力。
 */


//============================================================================
// 协议固定常量
//============================================================================

/** @brief 双字节帧头的第一个同步字节，用于从混合文本/二进制流中寻找候选起点。 */
#define IMAGE_REQUEST_SOF0              0xA5U

/** @brief 双字节帧头的第二个同步字节，降低单个 0xA5 被误判为完整帧头的概率。 */
#define IMAGE_REQUEST_SOF1              0x5AU

/** @brief 当前图像请求协议版本号。 */
#define IMAGE_REQUEST_VERSION           0x01U

/** @brief 请求一帧图像的消息类型。 */
#define IMAGE_REQUEST_MSG_REQUEST_IMAGE 0x20U

/** @brief 帧尾字节 0（CR）。 */
#define IMAGE_REQUEST_EOF0              0x0DU

/** @brief 帧尾字节 1（LF）。 */
#define IMAGE_REQUEST_EOF1              0x0AU

/** @brief V1 固定总长：2 字节 SOF + 6 字节业务字段 + 4 字节 CRC + 2 字节 EOF。 */
#define IMAGE_REQUEST_FRAME_SIZE        14U

/** @brief V1 只携带“请求一帧”字段，不在请求后附带 payload，因此固定为 0。 */
#define IMAGE_REQUEST_PAYLOAD_LEN_V1    0U

/** @brief 相邻字节最大间隔，防止残缺帧长期占用 dispatcher 的 BINARY 模式。 */
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
    IMAGE_REQUEST_PARSE_NONE = 0,           /**< 当前没有候选帧或没有需要上报的事件 */
    IMAGE_REQUEST_PARSE_PENDING,            /**< 已有活动候选帧，仍需后续字节才能判定 */
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
    uint32_t computed_crc;            /**< 尚未 Finalize 的增量 CRC 内部状态，仅累计 [2..7] */
    uint32_t received_crc;            /**< 按小端顺序从 [8..11] 重组的发送端 CRC */
    uint32_t last_byte_time_ms;       /**< 最近一个已接受字节时间，用于 inter-byte timeout */
    uint8_t frame_active;             /**< 是否正在接收一帧（1=活跃） */
} ImageRequestParser_t;

//============================================================================
// 解析器接口函数
//============================================================================

/**
 * @brief  初始化解析器并进入空闲同步状态
 * @param  parser 由调用者静态分配的解析器指针
 * @note   将状态设为 IMAGE_REQUEST_STATE_SYNC0，并重置所有内部字段。
 *         应由 CameraServiceTask 在 dispatcher 初始化阶段调用，不得与 FeedByte() 并发。
 */
void ImageRequestProtocol_Init(ImageRequestParser_t *parser);

/**
 * @brief  清除候选帧并恢复空闲同步状态
 * @param  parser 由调用者静态分配的解析器指针
 * @note   只清 parser 内部候选帧，不修改调用方此前保存的 out_frame。
 */
void ImageRequestProtocol_Reset(ImageRequestParser_t *parser);

/**
 * @brief  向解析器输入一个字节
 * @param  parser    解析器指针
 * @param  byte      当前输入的字节
 * @param  now_ms    调用者提供的当前毫秒时间戳
 * @param  out_frame 输出参数：仅在返回 IMAGE_REQUEST_PARSE_OK 时写入完整帧内容
 * @return 解析结果码（见 @ref ImageRequestParseResult_t）
 * @note   每次调用只消费一个字节。成功、字段错误或超时后 parser 会按状态复位或重同步。
 * @warning out_frame 必须是独立对象，不能指向 parser 内部 frame；成功复制后 parser
 *          会立即 Reset，若二者别名相同，刚输出的字段会被一并清零。
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
 * @note   CameraServiceTask 在 UART 有界读取未收到数据时调用。时间差使用无符号减法，
 *         可自然跨越 HAL tick 回绕；空闲状态返回 NONE，不会产生伪超时。
 */
ImageRequestParseResult_t ImageRequestProtocol_CheckTimeout(
    ImageRequestParser_t *parser,
    uint32_t now_ms);

/**
 * @brief  查询解析器是否正在接收候选帧
 * @param  parser 解析器指针
 * @retval 1 正在接收中（frame_active 为真）
 * @retval 0 空闲状态；parser 为 NULL 时也按防御性容错返回 0
 */
uint8_t ImageRequestProtocol_IsActive(const ImageRequestParser_t *parser);

#endif /* IMAGE_REQUEST_PROTOCOL_H */
