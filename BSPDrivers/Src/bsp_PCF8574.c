//
// Created by FAKE on 2026/5/27.
//
#include "bsp_PCF8574.h"
#include "i2c.h"

/* PCF8574 使用 CubeMX 配置的 I2C2：PH4=SCL、PH5=SDA。 */
#define PCF8574_I2C_HANDLE       hi2c2
#define PCF8574_I2C_TIMEOUT_MS   100u

/* PCF8574 为准双向端口，位写必须基于影子字节保留其他引脚状态；默认全部置高。 */
static uint8_t g_pcf8574_shadow = 0xFFu;

// 检测 PCF8574 是否在 I2C 总线上就绪
uint8_t PCF8574_IsReady(void)
{
    return (HAL_I2C_IsDeviceReady(&PCF8574_I2C_HANDLE,
                                  PCF8574_I2C_ADDR,
                                  3,
                                  PCF8574_I2C_TIMEOUT_MS) == HAL_OK) ? 0u : 1u;
}

// 向 PCF8574 写入完整字节，并在成功后更新本地影子值
uint8_t PCF8574_WriteByte(uint8_t data)
{
    if (HAL_I2C_Master_Transmit(&PCF8574_I2C_HANDLE,
                                PCF8574_I2C_ADDR,
                                &data,
                                1,
                                PCF8574_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return 1u;
    }

    // 仅在硬件写入成功后更新影子值，避免通信失败后续位操作基于错误状态。
    g_pcf8574_shadow = data;
    return 0u;
}

// 从 PCF8574 读取完整字节
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

// 基于影子值更新一个 PCF8574 引脚，并立即写入硬件
uint8_t PCF8574_WriteBit(uint8_t bit, uint8_t level)
{
    uint8_t new_value;

    if (bit > 7u)
    {
        return 1u;
    }

    new_value = g_pcf8574_shadow;
    if (level)
    {
        new_value |= (uint8_t)(1u << bit);
    }
    else
    {
        new_value &= (uint8_t)~(1u << bit);
    }

    return PCF8574_WriteByte(new_value);
}

// 读取一个 PCF8574 引脚的当前电平
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

// 检测 PCF8574 并将全部引脚初始化为高电平
uint8_t PCF8574_Init(void)
{
    if (PCF8574_IsReady() != 0u)
    {
        return 1u;
    }

    // OV_PWDN 为高有效，摄像头上电流程稍后会单独把 P2 拉低。
    if (PCF8574_WriteByte(0xFFu) != 0u)
    {
        return 1u;
    }

    return 0u;
}
