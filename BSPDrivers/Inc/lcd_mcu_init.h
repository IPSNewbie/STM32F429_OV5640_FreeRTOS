//
// Created by FAKE on 2026/6/4.
//

#ifndef ISP_OV5640_LCD_MCU_INIT_H
#define ISP_OV5640_LCD_MCU_INIT_H
#include "stm32f4xx_hal.h"
#include <stdint.h>

/**
 * @brief 执行 NT35310 厂家寄存器初始化序列
 * @note 仅由 LCD 主驱动在识别控制器后调用，主驱动逻辑位于 lcd_mcu.c。
 */
void LCD_MCU_NT35310_RegInit(void);


#endif //ISP_OV5640_LCD_MCU_INIT_H
