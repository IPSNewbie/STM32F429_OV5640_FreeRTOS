//
// Created by FAKE on 2026/5/27.
//

#ifndef ISP_OV5640_BSP_PCF8574_H
#define ISP_OV5640_BSP_PCF8574_H
#include "i2c.h"
#include "stm32f4xx_hal.h"

/**
 * @brief Apollo V2 板载 PCF8574 的 I2C 地址
 * @note PCF8574T 连接 I2C2（PH4=SCL、PH5=SDA）；HAL 使用左移一位后的 8 位地址 0x40。
 */
#define PCF8574_ADDR_7BIT          0x20u
#define PCF8574_I2C_ADDR           (PCF8574_ADDR_7BIT << 1)

/** @name PCF8574 位编号
 * @{ */
#define PCF8574_IO_P0              0u
#define PCF8574_IO_P1              1u
#define PCF8574_IO_P2              2u
#define PCF8574_IO_P3              3u
#define PCF8574_IO_P4              4u
#define PCF8574_IO_P5              5u
#define PCF8574_IO_P6              6u
#define PCF8574_IO_P7              7u
/** @} */

/** @brief OV5640 PWDN 在 Apollo V2 扩展 IO 上对应 P2。 */
#define PCF8574_OV_PWDN_IO         PCF8574_IO_P2

/**
 * @brief 初始化 PCF8574 并写入默认输出状态
 * @return 0-成功，1-I2C 访问失败
 */
uint8_t PCF8574_Init(void);

/**
 * @brief 读取 PCF8574 全部 8 位输入状态
 * @param data 接收端口状态的输出指针
 * @return 0-成功，1-参数非法或 I2C 访问失败
 */
uint8_t PCF8574_ReadByte(uint8_t *data);

/**
 * @brief 写入 PCF8574 全部 8 位输出状态
 * @param data 要写入的端口状态
 * @return 0-成功，1-I2C 访问失败
 */
uint8_t PCF8574_WriteByte(uint8_t data);

/**
 * @brief 设置 PCF8574 的单个位
 * @param bit 位编号，范围 0~7
 * @param level 目标电平，0-低电平，非 0-高电平
 * @return 0-成功，1-位编号非法或 I2C 访问失败
 */
uint8_t PCF8574_WriteBit(uint8_t bit, uint8_t level);

/**
 * @brief 读取 PCF8574 的单个位
 * @param bit 位编号，范围 0~7
 * @param level 接收电平的输出指针
 * @return 0-成功，1-参数非法或 I2C 访问失败
 */
uint8_t PCF8574_ReadBit(uint8_t bit, uint8_t *level);

/**
 * @brief 检查 PCF8574 是否响应 I2C 地址
 * @return 0-设备就绪，1-设备未就绪
 */
uint8_t PCF8574_IsReady(void);

#endif //ISP_OV5640_BSP_PCF8574_H
