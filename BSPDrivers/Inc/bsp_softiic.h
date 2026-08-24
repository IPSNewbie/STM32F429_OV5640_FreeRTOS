//
// Created by FAKE on 2026/5/28.
//

#ifndef ISP_OV5640_SOFTIIC_H
#define ISP_OV5640_SOFTIIC_H
#include "stm32f4xx_hal.h"

/** @brief SCCB 位操作的半周期延时，单位 us。 */
#define SCCB_DELAY_US   5

/** @brief 产生 SCCB 起始条件。 */
void MyI2C_Start(void);

/** @brief 产生 SCCB 停止条件。 */
void MyI2C_Stop(void);

/**
 * @brief 通过 SCCB 发送一个字节
 * @param Byte 待发送字节，高位先发
 */
void MyI2C_SendByte(uint8_t Byte);

/**
 * @brief 通过 SCCB 接收一个字节
 * @return 接收到的字节
 */
uint8_t MyI2C_ReceiveByte(void);

/**
 * @brief 发送 SCCB 应答位
 * @param AckBit 0-ACK，非 0-NACK
 */
void MyI2C_SendAck(uint8_t AckBit);

/**
 * @brief 接收 SCCB 应答位
 * @return 0-ACK，1-NACK
 */
uint8_t MyI2C_ReceiveAck(void);

#endif //ISP_OV5640_SOFTIIC_H
