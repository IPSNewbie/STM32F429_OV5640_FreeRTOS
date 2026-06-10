//
// Created by FAKE on 2026/6/4.
//

#ifndef ISP_OV5640_LCD_MCU_INIT_H
#define ISP_OV5640_LCD_MCU_INIT_H
#include "stm32f4xx_hal.h"
#include <stdint.h>

/*
 * 只声明 LCD 控制器厂家初始化函数。
 * 主驱动逻辑在 lcd_mcu.c。
 */

void LCD_MCU_NT35310_RegInit(void);


#endif //ISP_OV5640_LCD_MCU_INIT_H
