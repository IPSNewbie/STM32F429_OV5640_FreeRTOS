//
// Created by FAKE on 2026/5/27.
//

#ifndef ISP_OV5640_BSP_PCF8574_H
#define ISP_OV5640_BSP_PCF8574_H
#include "i2c.h"
#include "stm32f4xx_hal.h"

/*
 * Apollo V2 STM32F429 board:
 * PCF8574T is connected to STM32 hardware I2C2:
 *   PH4 -> I2C2_SCL
 *   PH5 -> I2C2_SDA
 *
 * PCF8574 7-bit address is usually 0x20 on this board,
 * so the HAL 8-bit address is 0x20 << 1 = 0x40.
 */
#define PCF8574_ADDR_7BIT          0x20u
#define PCF8574_I2C_ADDR           (PCF8574_ADDR_7BIT << 1)

#define PCF8574_IO_P0              0u
#define PCF8574_IO_P1              1u
#define PCF8574_IO_P2              2u
#define PCF8574_IO_P3              3u
#define PCF8574_IO_P4              4u
#define PCF8574_IO_P5              5u
#define PCF8574_IO_P6              6u
#define PCF8574_IO_P7              7u

/* OV_PWDN is connected to PCF8574 P2 on Apollo V2. */
#define PCF8574_OV_PWDN_IO         PCF8574_IO_P2

uint8_t PCF8574_Init(void);
uint8_t PCF8574_ReadByte(uint8_t *data);
uint8_t PCF8574_WriteByte(uint8_t data);
uint8_t PCF8574_WriteBit(uint8_t bit, uint8_t level);
uint8_t PCF8574_ReadBit(uint8_t bit, uint8_t *level);
uint8_t PCF8574_IsReady(void);

#endif //ISP_OV5640_BSP_PCF8574_H
