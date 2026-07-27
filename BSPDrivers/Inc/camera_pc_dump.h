#ifndef ISP_OV5640_CAMERA_PC_DUMP_H
#define ISP_OV5640_CAMERA_PC_DUMP_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

//============================================================================
// @file    isp_ov5640_camera_pc_dump.h
// @brief   OV5640 摄像头 PC 端图像导出模块（UART 传输）
// @note    用于将 160x120 RGB565 图像帧通过 UART 发送到 PC 上位机。
//          支持 "DUMP" 和 "AEC" 两个串口命令，数据包包含帧头、有效载荷和 CRC32 校验。
//============================================================================


//============================================================================
// 图像尺寸及数据包长度宏定义
//============================================================================

// 图像宽度（像素）
#define PC_DUMP_WIDTH       160U

// 图像高度（像素）
#define PC_DUMP_HEIGHT      120U

// DMA 传输所需的 32 位字数（总像素数 / 2，因两个 16 位像素组成一个 32 位字）
#define PC_DUMP_WORD_COUNT  (PC_DUMP_WIDTH * PC_DUMP_HEIGHT / 2U)

// 有效载荷字节数（图像数据总长度：宽度 × 高度 × 每像素 2 字节）
#define PC_DUMP_PAYLOAD_LEN (PC_DUMP_WIDTH * PC_DUMP_HEIGHT * 2U)

//============================================================================
// 串口命令码定义
//============================================================================

// 无命令
#define CAMERA_PC_DUMP_CMD_NONE  0U

// 要求传输一帧图像数据
#define CAMERA_PC_DUMP_CMD_DUMP  1U

// 要求打印 AEC/AGC 寄存器值（调试用）
#define CAMERA_PC_DUMP_CMD_AEC   2U

// 已处理完整的文本 CLI 命令
#define CAMERA_PC_DUMP_CMD_CLI   3U

// 已消耗一个命令字节，但行尚未完整接收
#define CAMERA_PC_DUMP_CMD_PENDING 4U

// UART 接收发生错误
#define CAMERA_PC_DUMP_CMD_UART_ERROR 5U

//============================================================================
// 公开函数声明
//============================================================================

/**
 * @brief  获取图像帧缓冲区的 32 位起始地址
 *         用于配置 DCMI DMA 的目标地址
 * @return 帧缓冲区首地址（指向后台缓冲区的指针）
 */
uint32_t Camera_PC_Dump_GetBufferAddress(void);

/**
 * @brief  获取图像帧缓冲区的字数（32 位为单位）
 *         用于配置 DCMI DMA 的传输数量
 * @return 固定值 PC_DUMP_WORD_COUNT（9600）
 */
uint32_t Camera_PC_Dump_GetWordCount(void);

/**
 * @brief  将一个接收字节送入现有文本命令行解析器
 * @param  huart UART 句柄，仅用于完整 CLI 命令的文本响应
 * @param  byte  本次输入字节
 * @retval CAMERA_PC_DUMP_CMD_DUMP    接收到完整的 "DUMP" 命令
 * @retval CAMERA_PC_DUMP_CMD_CLI     接收到完整的文本 CLI 命令并已处理
 * @retval CAMERA_PC_DUMP_CMD_PENDING 字节已消费，但命令行尚未完整
 * @retval CAMERA_PC_DUMP_CMD_NONE    参数无效
 * @note   本函数不读取 UART，只维护原有静态文本行状态。
 */
uint8_t Camera_PC_Dump_FeedCommandByte(UART_HandleTypeDef *huart, uint8_t byte);

/**
 * @brief  无输出地清空当前文本命令行状态
 * @note   用于 UART 错误恢复或 StreamBuffer 溢出后的重同步。
 */
void Camera_PC_Dump_ResetCommandParser(void);

// /**
//  * @brief  等待 PC 通过 UART 发送任意命令（DUMP 或 AEC）
//  * @param  huart 使用的 UART 句柄
//  * @retval CAMERA_PC_DUMP_CMD_DUMP  收到 "DUMP\n" 命令
//  * @retval CAMERA_PC_DUMP_CMD_AEC   收到 "AEC\n" 命令
//  * @retval CAMERA_PC_DUMP_CMD_NONE  参数无效（huart == NULL）或超时/错误
//  * @note   该函数会阻塞，逐字节接收 UART 数据。
//  *         忽略 '\r'，以 '\n' 为行结束符。
//  *         支持 "DUMP" 和 "AEC" 两个命令（不区分大小写）。
//  */
// uint8_t Camera_PC_Dump_WaitForCommand(UART_HandleTypeDef *huart);

/**
 * @brief  将一帧图像数据打包并通过 UART 发送给 PC
 * @param  huart    UART 句柄
 * @param  frame_id 帧序号，PC 端可用于排序或丢弃重复帧
 * @retval 0 发送成功
 * @retval 1 huart 为空
 * @retval 2 帧头发送失败
 * @retval 3 有效载荷分块发送失败
 * @retval 4 CRC 校验值发送失败
 * @retval 5 帧缓冲区状态或大小无效（数据指针为空或尺寸不匹配）
 * @note   数据格式（小端）：
 *         8B  魔数 "OV56RGB5"  (标识 OV5640 RGB565 帧)
 *         1B  版本号 = 1
 *         1B  保留 = 1
 *         2B  图像宽度 (LE)
 *         2B  图像高度 (LE)
 *         4B  有效载荷长度 (LE)  = PC_DUMP_PAYLOAD_LEN
 *         4B  帧 ID (LE)
 *         N*B 有效载荷（图像像素数据，RGB565 小端）
 *         4B  CRC32（针对有效载荷）
 */
uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id);

#endif /* ISP_OV5640_CAMERA_PC_DUMP_H */
