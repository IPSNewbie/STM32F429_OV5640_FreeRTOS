#ifndef ISP_OV5640_CAMERA_PC_DUMP_H
#define ISP_OV5640_CAMERA_PC_DUMP_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

#define PC_DUMP_WIDTH       160U
#define PC_DUMP_HEIGHT      120U
#define PC_DUMP_WORD_COUNT  (PC_DUMP_WIDTH * PC_DUMP_HEIGHT / 2U)
#define PC_DUMP_PAYLOAD_LEN (PC_DUMP_WIDTH * PC_DUMP_HEIGHT * 2U)

uint32_t Camera_PC_Dump_GetBufferAddress(void);
uint32_t Camera_PC_Dump_GetWordCount(void);
uint8_t Camera_PC_Dump_WaitForDumpCommand(UART_HandleTypeDef *huart);
uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id);

#endif /* ISP_OV5640_CAMERA_PC_DUMP_H */
