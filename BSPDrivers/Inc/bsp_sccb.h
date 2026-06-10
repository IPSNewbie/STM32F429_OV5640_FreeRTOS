//
// Created by FAKE on 2026/5/28.
//

#ifndef ISP_OV5640_BSP_SCCB_H
#define ISP_OV5640_BSP_SCCB_H
#include "stm32f4xx_hal.h"
#include <stdint.h>

#define OV5640_SCCB_ADDR_WRITE    0x78
#define OV5640_SCCB_ADDR_READ     0x79

uint8_t SCCB_WriteReg(uint16_t reg, uint8_t data);
uint8_t SCCB_ReadReg(uint16_t reg, uint8_t *data);

uint16_t OV5640_ReadID(void);

#endif //ISP_OV5640_BSP_SCCB_H
