//
// Created by FAKE on 2026/5/28.
//

#ifndef ISP_OV5640_BSP_SCCB_H
#define ISP_OV5640_BSP_SCCB_H
#include "stm32f4xx_hal.h"
#include <stdint.h>

/** @brief OV5640 SCCB 写地址。 */
#define OV5640_SCCB_ADDR_WRITE    0x78
/** @brief OV5640 SCCB 读地址。 */
#define OV5640_SCCB_ADDR_READ     0x79

/**
 * @brief 向 OV5640 的 16 位寄存器地址写入一个字节
 * @param reg 寄存器地址
 * @param data 待写入数据
 * @return 0-成功，1-SCCB 应答失败
 */
uint8_t SCCB_WriteReg(uint16_t reg, uint8_t data);

/**
 * @brief 从 OV5640 的 16 位寄存器地址读取一个字节
 * @param reg 寄存器地址
 * @param data 接收寄存器值的输出指针
 * @return 0-成功，1-参数非法或 SCCB 应答失败
 */
uint8_t SCCB_ReadReg(uint16_t reg, uint8_t *data);

/**
 * @brief 读取 OV5640 芯片 ID
 * @return 读取成功时返回 16 位芯片 ID，通信失败时返回 0xFFFF
 */
uint16_t OV5640_ReadID(void);

#endif //ISP_OV5640_BSP_SCCB_H
