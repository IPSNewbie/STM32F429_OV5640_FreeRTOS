#ifndef ISP_OV5640_CAMERA_PC_DUMP_H
#define ISP_OV5640_CAMERA_PC_DUMP_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

#define PC_DUMP_WIDTH       160U
#define PC_DUMP_HEIGHT      120U
#define PC_DUMP_WORD_COUNT  (PC_DUMP_WIDTH * PC_DUMP_HEIGHT / 2U)
#define PC_DUMP_PAYLOAD_LEN (PC_DUMP_WIDTH * PC_DUMP_HEIGHT * 2U)

#define CAMERA_PC_DUMP_CMD_NONE  0U
#define CAMERA_PC_DUMP_CMD_DUMP  1U
#define CAMERA_PC_DUMP_CMD_AEC   2U

/**
 * @brief  获取图像帧缓冲区的 32 位起始地址
 *         用于配置 DCMI DMA 的目标地址
 */
uint32_t Camera_PC_Dump_GetBufferAddress(void);

/**
 * @brief  获取图像帧缓冲区的字数（32 位为单位）
 *         用于配置 DCMI DMA 的传输数量
 */
uint32_t Camera_PC_Dump_GetWordCount(void);

uint8_t Camera_PC_Dump_WaitForCommand(UART_HandleTypeDef *huart);

/**
 * @brief  等待 PC 通过 UART 发送 "DUMP" 命令
 * @param  huart : 使用的 UART 句柄
 * @retval 1 : 收到有效 "DUMP" 命令
 *         0 : 参数无效（huart == NULL）
 * @note   该函数会阻塞，逐字节接收 UART 数据。
 *         忽略 '\r'，以 '\n' 为行结束符。
 *         当接收到 "DUMP\n" 时返回 1；其他行或错误后重置接收状态。
 */
uint8_t Camera_PC_Dump_WaitForDumpCommand(UART_HandleTypeDef *huart);

/**
 * @brief  将一帧图像数据打包并通过 UART 发送给 PC
 * @param  huart    : UART 句柄
 * @param  frame_id : 帧序号，PC 端可用于排序或丢弃重复帧
 * @retval 0 : 发送成功
 *         1 : huart 为空
 *         2 : 帧头发送失败
 *         3 : 有效载荷分块发送失败
 *         4 : CRC 校验值发送失败
 *         5 : frame buffer state or size invalid
 * @note   数据格式：
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
