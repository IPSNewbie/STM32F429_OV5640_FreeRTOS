//
// Created by FAKE on 2026/6/2.
//

#ifndef ISP_OV5640_OV5640_H
#define ISP_OV5640_OV5640_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

// OV5640 芯片 ID，正常应读到 0x5640
#define OV5640_MIN_ID       0x5640U

// 检查 OV5640 ID 是否正确
uint8_t OV5640_Min_CheckID(void);

// 初始化 OV5640 为 RGB565 + QVGA + 测试彩条输出
uint8_t OV5640_Min_InitRGB565_QVGA_TestBar(void);

// 打开/关闭 OV5640 内部测试彩条
uint8_t OV5640_Min_EnableTestBar(uint8_t enable);

// 读回关键寄存器，用于调试确认配置是否写入成功
uint8_t OV5640_Min_ReadBackDebug(void);
uint8_t OV5640_Min_ReadBackTimingDebug(const char *tag);
uint8_t OV5640_Min_OutSize_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);
uint8_t OV5640_Min_ImageWindow_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);

//关闭彩条测试，输出传感器图像至LCD
uint8_t OV5640_Min_InitRGB565_QVGA_RealImage(void);

uint8_t OV5640_Min_InitRGB565_480x320_TestBar(void);
uint8_t OV5640_Min_InitRGB565_480x320_RealImage(void);
#endif // ISP_OV5640_OV5640_H
