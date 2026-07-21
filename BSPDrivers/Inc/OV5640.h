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

/**
 * @brief  回读 OV5640 时序相关的全部寄存器，用于详细调试与确认配置完整性
 * @param  tag : 调用场景标签（例如 "init" 或 "run"），便于日志区分
 * @retval 0:成功, 1:某个寄存器读取失败
 * @note   列表涵盖 PLL、输入窗口、输出尺寸、时序、同步、格式等关键寄存器
 */
uint8_t OV5640_Min_ReadBackTimingDebug(const char *tag);

/**
 * @brief  设置 OV5640 缩放后的 DVP 输出尺寸，以及 ISP 内部的图像偏移
 * @param  offx  : X 方向 ISP 偏移量（像素）
 * @param  offy  : Y 方向 ISP 偏移量（像素）
 * @param  width : 最终输出图像的宽度（像素）
 * @param  height: 最终输出图像的高度（像素）
 * @retval 0:成功, 非0:对应寄存器写入失败
 */
uint8_t OV5640_Min_OutSize_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);

/**
 * @brief  设置 OV5640 传感器或 ISP 输入图像窗口（裁剪区域），该区域将被缩放至输出尺寸
 * @param  offx  : 输入窗口的起始 X 坐标（像素）
 * @param  offy  : 输入窗口的起始 Y 坐标（像素）
 * @param  width : 输入窗口的宽度（像素）
 * @param  height: 输入窗口的高度（像素）
 * @retval 0:成功, 非0:对应寄存器写入失败
 * @note   窗口结束坐标 = 起始坐标 + 宽度 - 1，长度不可为 0
 */
uint8_t OV5640_Min_ImageWindow_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);

/**
 * @brief  初始化 OV5640 为 RGB565 QVGA (320x240) 真实图像输出
 *         复用已验证的彩条测试初始化，再关闭彩条并开启自动曝光/增益
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_QVGA_RealImage(void);

/**
 * @brief  初始化 OV5640 为 RGB565 160x120 测试彩条输出
 *         复用 QVGA 彩条初始化，再通过 OV5640_Min_OutSize_Set 缩小输出尺寸
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_160x120_TestBar(void);

/**
 * @brief  初始化 OV5640 为 RGB565 160x120 真实图像输出
 *         复用 160x120 测试彩条初始化，再关闭彩条并开启自动曝光/增益
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_160x120_RealImage(void);

/**
 * @brief  初始化 OV5640 为 RGB565 480x320 测试彩条输出
 *         完整执行基础配置、RGB565 模式、图像窗口、输出尺寸与测试彩条开启
 * @retval 0:成功, 1:芯片ID检查失败, 2:基础初始化表写入失败,
 *         3:RGB565模式表写入失败, 4:图像窗口设置失败, 5:输出尺寸设置失败,
 *         8:输出格式设置失败, 9:测试彩条开启失败
 */
uint8_t OV5640_Min_InitRGB565_480x320_TestBar(void);

/**
 * @brief  初始化 OV5640 为 RGB565 480x320 真实图像输出
 *         复用测试彩条初始化流程，再关闭彩条并开启自动曝光/自动增益
 * @retval 0:成功, 其他:失败码（具体取决于调用的子函数）
 */
uint8_t OV5640_Min_InitRGB565_480x320_RealImage(void);
#endif // ISP_OV5640_OV5640_H
