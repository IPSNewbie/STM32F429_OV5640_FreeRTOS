#ifndef ISP_OV5640_CAMERA_PC_DUMP_H
#define ISP_OV5640_CAMERA_PC_DUMP_H

#include "stm32f4xx_hal.h"                             // 提供 UART_HandleTypeDef，供文本回复和图像帧发送接口使用
#include <stdint.h>                                    // 提供 uint8_t、uint16_t、uint32_t 等协议固定宽度整数类型

/**
 * @file camera_pc_dump.h
 * @brief OV5640 图像帧通过 UART 导出到 PC 的接口
 *
 * 本模块连接以下三条工程链路：
 *
 * 1. CaptureTask 通过本模块查询 DCMI DMA 的 back buffer 地址和 32 位传输数量；
 * 2. UART dispatcher 将文本字节交给本模块，DUMP 事件返回 CommTask；
 * 3. Camera RTOS 准备并提交 front frame 后，本模块按 OV56RGB5 协议发送图像。
 *
 * @note 文本解析状态由模块内部静态变量保存，只应由 CommTask 单一上下文调用。
 * @note 本模块不直接启动 DCMI，也不会把仍由 DMA 写入的 back buffer 发送给 PC。
 */


//============================================================================
// 图像尺寸及数据包长度
//============================================================================

/** @brief PC Dump 图像固定宽度，单位像素。 */
#define PC_DUMP_WIDTH       160U                       // OV56RGB5 当前正式输出固定为 160 像素宽

/** @brief PC Dump 图像固定高度，单位像素。 */
#define PC_DUMP_HEIGHT      120U                       // PC Dump 图像高度固定为 120 像素

/** @brief DCMI DMA 采集一帧所需的 32 位 word 数。 */
#define PC_DUMP_WORD_COUNT  \
    (PC_DUMP_WIDTH * PC_DUMP_HEIGHT / 2U)              // DCMI DMA 以 32 位 word 为单位传输；一个 word 可容纳两个 RGB565 像素

/** @brief 一帧 160x120 RGB565 有效载荷长度，单位字节。 */
#define PC_DUMP_PAYLOAD_LEN \
    (PC_DUMP_WIDTH * PC_DUMP_HEIGHT * 2U)              // RGB565 每个像素占 2 字节，因此图像数据总长度为 160×120×2=38400 字节


//============================================================================
// 文本命令解析结果
//============================================================================

/** @brief 当前没有得到有效文本命令结果。 */
#define CAMERA_PC_DUMP_CMD_NONE        0U              // 当前没有得到有效结果，通常表示参数无效或没有完整命令

/** @brief 已接收到完整 DUMP 命令。 */
#define CAMERA_PC_DUMP_CMD_DUMP        1U              // 已接收到完整 DUMP 命令，调用方应执行采集、处理和图像发送

/** @brief 历史 AEC 文本命令结果，当前最终 CLI 不再暴露该命令。 */
#define CAMERA_PC_DUMP_CMD_AEC         2U              // 已接收到完整 AEC 命令，用于输出 OV5640 AEC/AGC 寄存器信息

/** @brief 普通 CLI 命令已完成解析并提交 CommandQueue。 */
#define CAMERA_PC_DUMP_CMD_CLI         3U              // 普通 CLI 命令已由 CLI parser 完成校验和提交

/** @brief 当前文本命令行尚未接收完整。 */
#define CAMERA_PC_DUMP_CMD_PENDING     4U              // 当前字节已保存，但一整行命令尚未接收完，需要继续输入后续字节

/** @brief UART 接收错误兼容状态。 */
#define CAMERA_PC_DUMP_CMD_UART_ERROR  5U              // UART 接收过程中发生错误；当前 DMA版本中主要作为兼容状态保留


//============================================================================
// 图像采集缓冲区接口
//============================================================================

/**
 * @brief  获取 DCMI DMA 本次采集使用的目标缓冲区地址
 * @return 后台缓冲区的起始地址，转换为 uint32_t 后供 HAL_DCMI_Start_DMA() 使用
 *
 * @note
 * 当前项目使用双缓冲：
 *
 * DCMI DMA -> back buffer -> commit/swap -> front buffer -> 图像处理/发送
 *
 * DCMI 只能写 back buffer，UART 发送读取 front buffer，
 * 从而避免采集过程中图像数据被发送任务同时读取。
 */
uint32_t Camera_PC_Dump_GetBufferAddress(void);         // 返回当前 back buffer 的地址，供 DCMI DMA 写入一帧图像


/**
 * @brief  获取 DCMI DMA 需要传输的 32 位 word 数量
 * @return PC_DUMP_WORD_COUNT，本项目固定为 9600 个 32 位 word
 *
 * @note
 * 160 × 120 = 19200 个像素；
 * 每个 RGB565 像素占 16 位；
 * 两个 RGB565 像素正好占一个 32 位 word；
 * 所以 DMA 传输数量为 19200 / 2 = 9600。
 */
uint32_t Camera_PC_Dump_GetWordCount(void);             // 返回 DCMI DMA 的传输数量，而不是字节数


//============================================================================
// 文本命令解析接口
//============================================================================

/**
 * @brief  将一个已经接收到的 UART 字节送入文本命令行解析器
 *
 * @param  huart
 *         UART 句柄。
 *         本函数本身不通过 HAL_UART_Receive() 读取字节；
 *         为保持现有接口而保留；解析阶段不进行 UART 输出。
 *
 * @param  byte
 *         当前从 StreamBuffer 中取出的一个字节。
 *
 * @retval CAMERA_PC_DUMP_CMD_DUMP
 *         已接收到完整的 DUMP 命令。
 *         CommTask 应将命令提交 CommandQueue，由 ControlTask 执行统一的图像采集与发送流程。
 *
 * @retval CAMERA_PC_DUMP_CMD_CLI
 *         已接收到完整的普通 CLI 命令，例如 HELP、STATUS、PROC 或 THR，
 *         并且命令已经由 camera_cli 模块完成解析并提交 CommandQueue。
 *
 * @retval CAMERA_PC_DUMP_CMD_PENDING
 *         当前字节属于一条尚未完成的文本命令，
 *         例如只收到了 "HEL"，还需要继续接收 "P\r\n"。
 *
 * @retval CAMERA_PC_DUMP_CMD_NONE
 *         参数无效、收到空行，或者本次没有形成需要上层处理的结果。
 *
 * @note
 * 当前 UART 接收链路为：
 *
 * USART1 RX
 *     -> DMA Circular
 *     -> HT / TC / IDLE 回调
 *     -> StreamBuffer
 *     -> CommTask
 *     -> CameraUartDispatcher
 *     -> Camera_PC_Dump_FeedCommandByte()
 *
 * 因此，本函数只负责文本行状态，不再直接读取 UART 硬件。
 *
 * @note 解析器使用模块内部静态状态，不可由多个任务或 ISR 并发调用。
 * @note DUMP 只形成返回事件；CommTask 将其转为命令，ControlTask 在出队后执行。
 */
uint8_t Camera_PC_Dump_FeedCommandByte(
    UART_HandleTypeDef *huart,                          // 保留的 USART1 句柄；解析阶段不发送文本
    uint8_t byte);                                      // 当前需要交给文本解析器处理的一个字节


/**
 * @brief  清空当前文本命令解析状态
 *
 * @note
 * 该函数通常在以下情况调用：
 *
 * 1. UART DMA 接收发生错误并恢复；
 * 2. StreamBuffer 溢出，需要丢弃残留数据；
 * 3. 当前文本行只接收到一部分，不能再继续使用；
 * 4. 系统需要重新同步 UART 输入。
 *
 * 调用后会清除：
 *
 * 1. 当前已保存的文本行内容；
 * 2. 当前文本行长度；
 * 3. 超长命令丢弃状态；
 * 4. 其他与当前未完成命令相关的静态状态。
 *
 * 本函数不会向 UART 输出任何文本。
 * 它只清除本模块的文本行状态；UART dispatcher 和 binary request parser
 * 由调用方通过各自的 Reset 接口另行清除。
 */
void Camera_PC_Dump_ResetCommandParser(void);           // 放弃当前未完成文本命令，使下一字节从新命令开始解析


//============================================================================
// 已停用的旧轮询接收接口
//============================================================================

// /**
//  * @brief  使用阻塞式 HAL_UART_Receive() 等待完整 UART 命令
//  *
//  * @param  huart
//  *         要使用的 UART 句柄。
//  *
//  * @retval CAMERA_PC_DUMP_CMD_DUMP
//  *         收到完整的 DUMP 命令。
//  *
//  * @retval CAMERA_PC_DUMP_CMD_AEC
//  *         收到完整的 AEC 命令。
//  *
//  * @retval CAMERA_PC_DUMP_CMD_NONE
//  *         参数无效、超时或 UART 接收错误。
//  *
//  * @note
//  * 这是项目早期裸机阶段使用的接口。
//  *
//  * 当时程序通过 HAL_UART_Receive() 每次读取一个字节。
//  * 加入 FreeRTOS 后，如果收到部分命令便执行 osDelay()，
//  * 后续字节可能在任务延时期间到达并造成 ORE 或字节丢失。
//  *
//  * 目前已经改为：
//  *
//  * UART DMA + IDLE + StreamBuffer + CommTask。
//  *
//  * 因此旧接口被注释保留，仅用于理解项目演进过程。
//  */
// uint8_t Camera_PC_Dump_WaitForCommand(
//     UART_HandleTypeDef *huart);                       // 旧版阻塞式命令接收函数，当前正式链路已经停用


//============================================================================
// OV56RGB5 图像帧发送接口
//============================================================================

/**
 * @brief  将 front buffer 中的一帧 RGB565 图像封装后通过 UART 发送到 PC
 *
 * @param  huart
 *         用于发送图像数据的 UART 句柄，当前项目使用 USART1。
 *
 * @param  frame_id
 *         STM32 端图像帧序号。
 *         每成功发送一帧后，上层通常将 frame_id 增加 1。
 *
 * @retval 0
 *         帧头、图像载荷和 CRC32 均发送成功。
 *
 * @retval 1
 *         huart 为空，无法执行 UART 发送。
 *
 * @retval 2
 *         22 字节 OV56RGB5 帧头发送失败。
 *
 * @retval 3
 *         38400 字节 RGB565 有效载荷分块发送失败。
 *
 * @retval 4
 *         图像末尾的 4 字节 CRC32 发送失败。
 *
 * @retval 5
 *         front buffer 无效，例如：
 *         - 数据指针为空；
 *         - 图像宽度不是 160；
 *         - 图像高度不是 120；
 *         - 图像长度不是 38400 字节。
 *
 * @note
 * 本函数发送的是现有 OV56RGB5 响应协议。
 *
 * 完整帧结构如下：
 *
 * 偏移 0：
 *     8 B magic，固定为 ASCII 字符串 "OV56RGB5"。
 *
 * 偏移 8：
 *     1 B version，当前固定为 1。
 *
 * 偏移 9：
 *     1 B pixel_format，当前值为 1，表示 RGB565。
 *
 * 偏移 10：
 *     2 B width，小端序，当前固定为 160。
 *
 * 偏移 12：
 *     2 B height，小端序，当前固定为 120。
 *
 * 偏移 14：
 *     4 B payload_len，小端序，当前固定为 38400。
 *
 * 偏移 18：
 *     4 B frame_id，小端序。
 *
 * 偏移 22：
 *     38400 B RGB565 图像有效载荷。
 *
 * 偏移 38422：
 *     4 B CRC32，小端序，只校验 38400 B 图像有效载荷。
 *
 * 完整帧总长度：
 *
 *     22 + 38400 + 4 = 38426 B
 *
 * @note
 * 二进制图像请求中的 seq 与这里的 frame_id 相互独立：
 *
 * seq：
 *     由 PC 端生成，用于标识请求。
 *
 * frame_id：
 *     由 STM32 维护，用于标识成功发送的图像帧。
 *
 * 当前协议不要求 seq 等于 frame_id。
 *
 * @warning 本函数使用阻塞式 HAL_UART_Transmit()，且 timeout 参数为 HAL_MAX_DELAY，
 *          模块本身没有额外的软件超时机制，不能在 ISR 中调用。
 * @note 调用前必须已经通过 Camera RTOS 的统一准备路径得到尺寸正确、已经 commit 的 front frame。
 * @note frame_id 的递增由上层在发送成功后完成；本函数只把传入值写入 header。
 */
uint8_t Camera_PC_Dump_SendFrame(
    UART_HandleTypeDef *huart,                          // UART发送句柄，当前为USART1
    uint32_t frame_id);                                 // 本次发送的STM32图像帧编号


#endif /* ISP_OV5640_CAMERA_PC_DUMP_H */                // 头文件保护结束
