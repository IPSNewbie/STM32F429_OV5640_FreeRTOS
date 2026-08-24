//
// Created by FAKE on 2026/5/28.
//

#ifndef ISP_OV5640_DELAY_TIM_H
#define ISP_OV5640_DELAY_TIM_H
#include "stm32f4xx_hal.h"

/**
 * @brief 初始化 TIM7 微秒延时基准
 * @note 调用 delay_us() 前需先完成初始化。
 */
void Delay_TIM7_Init(void);

/**
 * @brief 使用 TIM7 进行微秒级忙等待
 * @param us 延时时间，单位 us
 * @note 该接口用于 SCCB 等短硬件时序，不应替代 RTOS 任务阻塞。
 */
void delay_us(uint16_t us);

#endif //ISP_OV5640_DELAY_TIM_H
