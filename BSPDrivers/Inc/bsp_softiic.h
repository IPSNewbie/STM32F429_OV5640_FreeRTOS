//
// Created by FAKE on 2026/5/28.
//

#ifndef ISP_OV5640_SOFTIIC_H
#define ISP_OV5640_SOFTIIC_H
#include "stm32f4xx_hal.h"

#define SCCB_DELAY_US   5
void MyI2C_Start(void);
void MyI2C_Stop(void);
void MyI2C_SendByte(uint8_t Byte);
uint8_t MyI2C_ReceiveByte(void);
void MyI2C_SendAck(uint8_t AckBit);
uint8_t MyI2C_ReceiveAck(void);

#endif //ISP_OV5640_SOFTIIC_H
