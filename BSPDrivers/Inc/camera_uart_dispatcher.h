#ifndef CAMERA_UART_DISPATCHER_H
#define CAMERA_UART_DISPATCHER_H

#include <stdint.h>  // 提供 UART 字节、毫秒时间戳及状态字段的固定宽度类型

#include "image_request_protocol.h"  // 固定 14 字节请求帧的 parser、字段和结果码

/**
 * @file camera_uart_dispatcher.h
 * @brief UART CLI 文本与 binary image request 的逐字节分发接口
 *
 * UART RX DMA ISR 只向 StreamBuffer 写入字节；CameraServiceTask 按接收顺序调用本模块。
 * IDLE 判定输入类别，TEXT 保持到 LF，BINARY 保持到请求完成、出错或超时。
 * 字段错误后的旧帧尾会被隔离，避免 CRC/EOF 字节泄漏为 CLI 文本。
 * @note 上下文由 CameraServiceTask 单一持有，不支持多任务或 ISR 并发访问。
 */

/**
 * @brief UART 应用层分发器当前模式
 */
typedef enum
{
    CAMERA_UART_DISPATCH_MODE_IDLE = 0, /**< 等待下一字节判定文本或 SOF0 */
    CAMERA_UART_DISPATCH_MODE_TEXT,     /**< 已进入 CLI 行，保持到 '\n' */
    CAMERA_UART_DISPATCH_MODE_BINARY   /**< 正在解析请求或隔离错误帧尾 */
} CameraUartDispatchMode_t;

/**
 * @brief 单次分发调用产生的事件类型
 */
typedef enum
{
    CAMERA_UART_DISPATCH_NONE = 0,       /**< 本次没有上层事件 */
    CAMERA_UART_DISPATCH_TEXT_BYTE,      /**< 输出一个文本字节 */
    CAMERA_UART_DISPATCH_IMAGE_REQUEST,  /**< 得到完整合法图像请求 */
    CAMERA_UART_DISPATCH_BINARY_ERROR,   /**< 二进制请求解析失败 */
    CAMERA_UART_DISPATCH_BINARY_TIMEOUT, /**< 二进制候选帧超时 */
    CAMERA_UART_DISPATCH_BAD_ARGUMENT    /**< 参数非法 */
} CameraUartDispatchResult_t;

/**
 * @brief UART 分发事件数据
 * @note 仅应读取与 type 对应的字段；未使用字段会被清零。
 */
typedef struct
{
    CameraUartDispatchResult_t type;         /**< 事件类型 */
    uint8_t text_byte;                       /**< 文本事件对应的原始字节 */
    ImageRequestFrame_t image_request;       /**< 合法二进制请求字段 */
    ImageRequestParseResult_t binary_result; /**< 二进制解析结果 */
} CameraUartDispatchEvent_t;

/**
 * @brief UART 分发器持久运行上下文
 * @note 由 CameraServiceTask 静态持有并跨字节保存，不支持并发访问。
 */
typedef struct
{
    CameraUartDispatchMode_t mode;      /**< 当前 IDLE、TEXT 或 BINARY 分类状态 */
    ImageRequestParser_t binary_parser; /**< 底层固定 14 字节请求解析上下文 */
    uint8_t binary_discard_active;      /**< 是否正在隔离错误帧尾部 */
    uint8_t binary_discard_remaining;   /**< 当前错误帧尚需丢弃的字节数 */
    uint32_t binary_discard_last_time_ms; /**< 最近一次隔离字节的时间戳 */
} CameraUartDispatcher_t;

/**
 * @brief 初始化 UART 分发器上下文
 * @param dispatcher 分发器上下文；空指针时直接返回
 */
void CameraUartDispatcher_Init(CameraUartDispatcher_t *dispatcher);

/**
 * @brief 丢弃当前文本或二进制候选帧并恢复空闲模式
 * @param dispatcher 分发器上下文；空指针时直接返回
 */
void CameraUartDispatcher_Reset(CameraUartDispatcher_t *dispatcher);

/**
 * @brief 向分发器输入一个 UART 字节
 * @param dispatcher 分发器上下文
 * @param byte 当前字节
 * @param now_ms 当前毫秒时间戳
 * @param out_event 接收本次事件的输出结构
 * @return 本次分发结果，见 @ref CameraUartDispatchResult_t
 * @note 每次只消费一个字节并产生至多一个事件；错误旧帧尾返回 NONE 并静默隔离。
 * @warning 仅限 CameraServiceTask 任务上下文，不得与 Reset/CheckTimeout 并发调用。
 */
CameraUartDispatchResult_t CameraUartDispatcher_FeedByte(
    CameraUartDispatcher_t *dispatcher,
    uint8_t byte,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event);

/**
 * @brief 检查二进制候选帧或错误尾部隔离是否超时
 * @param dispatcher 分发器上下文
 * @param now_ms 当前毫秒时间戳
 * @param out_event 接收本次事件的输出结构
 * @return 超时、等待中或无事件等分发结果
 * @note 检查的是相邻字节间隔，不是整帧累计时间；尾部隔离超时只恢复 IDLE。
 */
CameraUartDispatchResult_t CameraUartDispatcher_CheckTimeout(
    CameraUartDispatcher_t *dispatcher,
    uint32_t now_ms,
    CameraUartDispatchEvent_t *out_event);

/**
 * @brief 查询分发器当前模式
 * @param dispatcher 分发器上下文
 * @return 当前模式；空指针时防御性返回空闲模式
 * @note 返回的是实时内部状态，不构成跨任务同步快照。
 */
CameraUartDispatchMode_t CameraUartDispatcher_GetMode(
    const CameraUartDispatcher_t *dispatcher);

#endif /* CAMERA_UART_DISPATCHER_H */
