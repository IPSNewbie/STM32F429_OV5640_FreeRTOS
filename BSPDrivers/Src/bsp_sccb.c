//
// Created by FAKE on 2026/5/28.
//
#include "bsp_sccb.h"
#include "bsp_softiic.h"
#include "bsp_log.h"

/* OV5640 ID字节细节日志开关：默认关闭，仅控制成功打印。 */
#ifndef SCCB_VERBOSE_ID_LOG
#define SCCB_VERBOSE_ID_LOG 0U
#endif

/**
 * @brief  SCCB写OV5640寄存器
 * @param  reg:  OV5640 16位寄存器地址
 * @param  data: 要写入的8位数据
 * @retval 0: 成功
 *         1: 失败
 */
uint8_t SCCB_WriteReg(uint16_t reg, uint8_t data)
{
    uint8_t ack = 0;

    MyI2C_Start();

    /*
     * 1. 发送设备写地址 0x78
     */
    MyI2C_SendByte(OV5640_SCCB_ADDR_WRITE);
    ack |= MyI2C_ReceiveAck();

    /*
     * 2. 发送寄存器地址高8位
     */
    MyI2C_SendByte((uint8_t)(reg >> 8));
    ack |= MyI2C_ReceiveAck();

    /*
     * 3. 发送寄存器地址低8位
     */
    MyI2C_SendByte((uint8_t)(reg & 0xFF));
    ack |= MyI2C_ReceiveAck();

    /*
     * 4. 发送寄存器数据
     */
    MyI2C_SendByte(data);
    ack |= MyI2C_ReceiveAck();

    MyI2C_Stop();

    if (ack == 0)
    {
        return 0;   // 成功
    }
    else
    {
        return 1;   // 失败
    }
}

/**
 * @brief  SCCB读OV5640寄存器
 * @param  reg:  OV5640 16位寄存器地址
 * @param  data: 读取到的数据存放地址
 * @retval 0: 成功
 *         1: 失败
 */
uint8_t SCCB_ReadReg(uint16_t reg, uint8_t *data)
{
    uint8_t ack = 0;

    if (data == NULL)
    {
        return 1;
    }

    /*
     * 第一步：写入要读取的寄存器地址
     */
    MyI2C_Start();

    MyI2C_SendByte(OV5640_SCCB_ADDR_WRITE);
    ack |= MyI2C_ReceiveAck();

    MyI2C_SendByte((uint8_t)(reg >> 8));
    ack |= MyI2C_ReceiveAck();

    MyI2C_SendByte((uint8_t)(reg & 0xFF));
    ack |= MyI2C_ReceiveAck();

    MyI2C_Stop();

    if (ack != 0)
    {
        return 1;
    }

    /*
     * 第二步：重新启动，读取寄存器数据
     */
    MyI2C_Start();

    MyI2C_SendByte(OV5640_SCCB_ADDR_READ);
    ack = MyI2C_ReceiveAck();

    if (ack != 0)
    {
        MyI2C_Stop();
        return 1;
    }

    *data = MyI2C_ReceiveByte();

    /*
     * 只读取1个字节，主机最后发送NACK
     */
    MyI2C_SendAck(1);

    MyI2C_Stop();

    return 0;
}

// 读取 OV5640 芯片 ID，并在任一 SCCB 访问失败时返回无效值
uint16_t OV5640_ReadID(void)
{
    uint8_t id_high = 0;
    uint8_t id_low  = 0;

    // if (SCCB_ReadReg(0x300A, &id_high) != 0)
    // {
    //     return 0xFFFF;
    // }
    //
    // if (SCCB_ReadReg(0x300B, &id_low) != 0)
    // {
    //     return 0xFFFF;
    // }
    uint8_t ret = 0;

    ret = SCCB_ReadReg(0x300A, &id_high);
    if (ret != 0)
    {
        LOG_ERROR("Read OV5640 ID high failed, ret = %d", ret);
        return 0xFFFF;
    }

    ret = SCCB_ReadReg(0x300B, &id_low);
    if (ret != 0)
    {
        LOG_ERROR("Read OV5640 ID low failed, ret = %d", ret);
        return 0xFFFF;
    }

#if (SCCB_VERBOSE_ID_LOG != 0U)
    LOG_DEBUG("OV5640 IDH = 0x%02X, IDL = 0x%02X", id_high, id_low);
#endif

    return ((uint16_t)id_high << 8) | id_low;
}
