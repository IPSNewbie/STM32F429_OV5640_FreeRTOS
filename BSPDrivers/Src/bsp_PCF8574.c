//
// Created by FAKE on 2026/5/27.
//
#include "bsp_PCF8574.h"
#include "i2c.h"

/*
 * CubeMX requirement:
 *   Enable I2C2 on PH4/PH5.
 *   Make sure i2c.c exports I2C_HandleTypeDef hi2c2;
 */
#define PCF8574_I2C_HANDLE       hi2c2
#define PCF8574_I2C_TIMEOUT_MS   100u

/*
 * PCF8574 is quasi-bidirectional. Keep an output shadow byte for output pins.
 * Default all high. OV_PWDN is high-active, so ov5640.c later pulls P2 low.
 */
static uint8_t g_pcf8574_shadow = 0xFFu;

/**
 * @brief  检测 PCF8574 是否在 I2C 总线上就绪
 * @param  无
 * @retval 0: 设备就绪，1: 设备未就绪或通信失败
 */
uint8_t PCF8574_IsReady(void)
{
    return (HAL_I2C_IsDeviceReady(&PCF8574_I2C_HANDLE,
                                  PCF8574_I2C_ADDR,
                                  3,
                                  PCF8574_I2C_TIMEOUT_MS) == HAL_OK) ? 0u : 1u;
}

/**
 * @brief  向 PCF8574 写入一个完整的 8 位数据，并更新本地影子寄存器
 * @param  data: 要写入的 8 位数据
 * @retval 0: 写入成功，1: 写入失败
 */
uint8_t PCF8574_WriteByte(uint8_t data)
{
    g_pcf8574_shadow = data;

    return (HAL_I2C_Master_Transmit(&PCF8574_I2C_HANDLE,
                                    PCF8574_I2C_ADDR,
                                    &data,
                                    1,
                                    PCF8574_I2C_TIMEOUT_MS) == HAL_OK) ? 0u : 1u;
}

/**
 * @brief  从 PCF8574 读取一个完整的 8 位数据
 * @param  data: 用于存放读取结果的指针（输出）
 * @retval 0: 读取成功，1: 读取失败（指针为空或 I2C 通信错误）
 */
uint8_t PCF8574_ReadByte(uint8_t *data)
{
    if (data == NULL)
    {
        return 1u;
    }

    return (HAL_I2C_Master_Receive(&PCF8574_I2C_HANDLE,
                                   PCF8574_I2C_ADDR,
                                   data,
                                   1,
                                   PCF8574_I2C_TIMEOUT_MS) == HAL_OK) ? 0u : 1u;
}

/**
 * @brief  设置 PCF8574 的某个 I/O 引脚电平，并立即写入硬件
 * @param  bit:   引脚编号（0～7）
 * @param  level: 期望电平（0 为低，非 0 为高）
 * @retval 0: 写入成功，1: 引脚号无效或 I2C 写入失败
 */
uint8_t PCF8574_WriteBit(uint8_t bit, uint8_t level)
{
    if (bit > 7u)
    {
        return 1u;
    }

    if (level)
    {
        g_pcf8574_shadow |= (uint8_t)(1u << bit);
    }
    else
    {
        g_pcf8574_shadow &= (uint8_t)~(1u << bit);
    }

    return PCF8574_WriteByte(g_pcf8574_shadow);
}

/**
 * @brief  读取 PCF8574 的某个 I/O 引脚当前电平
 * @param  bit:   引脚编号（0～7）
 * @param  level: 用于存放读取电平的输出指针（1 为高，0 为低）
 * @retval 0: 读取成功，1: 引脚号无效、指针为空或 I2C 通信失败
 */
uint8_t PCF8574_ReadBit(uint8_t bit, uint8_t *level)
{
    uint8_t data = 0;

    if ((bit > 7u) || (level == NULL))
    {
        return 1u;
    }

    if (PCF8574_ReadByte(&data) != 0u)
    {
        return 1u;
    }

    *level = (data & (uint8_t)(1u << bit)) ? 1u : 0u;
    return 0u;
}

/**
 * @brief  初始化 PCF8574，检测设备是否就绪并将所有引脚初始化为高电平
 * @param  无
 * @retval 0: 初始化成功，1: 设备未就绪或写入失败
 */
uint8_t PCF8574_Init(void)
{
    if (PCF8574_IsReady() != 0u)
    {
        return 1u;
    }

    /* Default all pins high. Camera power sequence will pull P2 low later. */
    if (PCF8574_WriteByte(0xFFu) != 0u)
    {
        return 1u;
    }

    return 0u;
}