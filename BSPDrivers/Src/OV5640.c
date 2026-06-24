//
// Created by FAKE on 2026/6/2.
//
#include "ov5640.h"
#include "bsp_sccb.h"
#include "bsp_log.h"
#include "ov5640cfg.h"

/*
 * 本文件只做“最小可视化调试配置”。
 * 当前目标：RGB565 + QVGA/480x320/320x240/160x120 + 彩条/RealImage
 * 复杂画质、曝光、AWB、AF 后面再接完整寄存器表。
 */

// 写 OV5640 单个寄存器
static uint8_t OV5640_Min_WriteReg(uint16_t reg, uint8_t val)
{
    // 通过 SCCB 写 16 位寄存器地址 + 8 位数据
    uint8_t ret = SCCB_WriteReg(reg, val);

    // 写失败则打印寄存器地址和值，方便定位
    if (ret != 0)
    {
        LOG_ERROR("OV5640 write failed: reg=0x%04X, val=0x%02X", reg, val);
        return 1;
    }

    // 每次写寄存器后稍作延时，保证传感器内部配置稳定
    HAL_Delay(1);

    return 0;
}

// 读 OV5640 单个寄存器
static uint8_t OV5640_Min_ReadReg(uint16_t reg, uint8_t *val)
{
    // 通过 SCCB 读取指定寄存器
    uint8_t ret = SCCB_ReadReg(reg, val);

    // 读失败则打印寄存器地址
    if (ret != 0)
    {
        LOG_ERROR("OV5640 read failed: reg=0x%04X", reg);
        return 1;
    }

    return 0;
}

// 批量写寄存器表
static uint8_t OV5640_Min_WriteTable(const uint16_t (*tbl)[2], uint32_t len)
{
    // 表格式：{寄存器地址, 寄存器值}
    for (uint32_t i = 0; i < len; i++)
    {
        // 逐项写入寄存器表
        if (OV5640_Min_WriteReg(tbl[i][0], (uint8_t)tbl[i][1]) != 0)
        {
            LOG_ERROR("OV5640 table write failed at index=%lu, reg=0x%04X, val=0x%02X",
                      i, tbl[i][0], (uint8_t)tbl[i][1]);
            return 1;
        }
    }

    return 0;
}

// 检查 OV5640 芯片 ID
uint8_t OV5640_Min_CheckID(void)
{
    // 读取 0x300A 和 0x300B，组合成芯片 ID
    uint16_t id = OV5640_ReadID();

    // 正常情况下应为 0x5640
    if (id == OV5640_MIN_ID)
    {
        return 0;
    }

    LOG_ERROR("OV5640 ID error: 0x%04X", id);
    return 1;
}

// 打开或关闭 OV5640 内部测试彩条
uint8_t OV5640_Min_EnableTestBar(uint8_t enable)
{
    // 0x4741 是测试图相关寄存器
    // bit[2] = 1：开启测试图
    // bit[0] = 1：选择 8-bit 测试图输出
    // 0x05 = bit[2] + bit[0]
    return OV5640_Min_WriteReg(0x4741, enable ? 0x05 : 0x00);
}

// 设置缩放后的 DVP 输出尺寸和 ISP 偏移
uint8_t OV5640_Min_OutSize_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height)
{
    // 配置 ISP 控制寄存器，允许修改相关参数（解锁写保护）
    if (SCCB_WriteReg(0x3212, 0x03) != 0) return 1;

    // 设置最终输出图像的宽度（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3808, (uint8_t)(width >> 8)) != 0) return 2;
    if (SCCB_WriteReg(0x3809, (uint8_t)(width & 0xFF)) != 0) return 3;

    // 设置最终输出图像的高度（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x380A, (uint8_t)(height >> 8)) != 0) return 4;
    if (SCCB_WriteReg(0x380B, (uint8_t)(height & 0xFF)) != 0) return 5;

    // 设置 ISP 内部 X 方向偏移（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3810, (uint8_t)(offx >> 8)) != 0) return 6;
    if (SCCB_WriteReg(0x3811, (uint8_t)(offx & 0xFF)) != 0) return 7;

    // 设置 ISP 内部 Y 方向偏移（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3812, (uint8_t)(offy >> 8)) != 0) return 8;
    if (SCCB_WriteReg(0x3813, (uint8_t)(offy & 0xFF)) != 0) return 9;

    // 锁定参数并启动 ISP 处理
    if (SCCB_WriteReg(0x3212, 0x13) != 0) return 10;
    if (SCCB_WriteReg(0x3212, 0xA3) != 0) return 11;

    return 0;
}

// 设置输出缩放前使用的传感器/ISP 图像窗口
uint8_t OV5640_Min_ImageWindow_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height)
{
    // 计算输入窗口的起始和结束坐标
    uint16_t xst = offx;
    uint16_t yst = offy;
    uint16_t xend = offx + width - 1;
    uint16_t yend = offy + height - 1;

    // 配置 ISP 控制寄存器，允许修改相关参数
    if (SCCB_WriteReg(0x3212, 0x03) != 0) return 1;

    // 设置输入窗口起始 X 坐标（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3800, (uint8_t)(xst >> 8)) != 0) return 2;
    if (SCCB_WriteReg(0x3801, (uint8_t)(xst & 0xFF)) != 0) return 3;

    // 设置输入窗口起始 Y 坐标（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3802, (uint8_t)(yst >> 8)) != 0) return 4;
    if (SCCB_WriteReg(0x3803, (uint8_t)(yst & 0xFF)) != 0) return 5;

    // 设置输入窗口结束 X 坐标（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3804, (uint8_t)(xend >> 8)) != 0) return 6;
    if (SCCB_WriteReg(0x3805, (uint8_t)(xend & 0xFF)) != 0) return 7;

    // 设置输入窗口结束 Y 坐标（高 8 位和低 8 位）
    if (SCCB_WriteReg(0x3806, (uint8_t)(yend >> 8)) != 0) return 8;
    if (SCCB_WriteReg(0x3807, (uint8_t)(yend & 0xFF)) != 0) return 9;

    // 锁定参数并启动 ISP 处理
    if (SCCB_WriteReg(0x3212, 0x13) != 0) return 10;
    if (SCCB_WriteReg(0x3212, 0xA3) != 0) return 11;

    return 0;
}


// 初始化 OV5640 为 RGB565 + QVGA + 测试彩条
uint8_t OV5640_Min_InitRGB565_QVGA_TestBar(void)
{
    // 先确认 SCCB 通信和芯片 ID 正常
    if (OV5640_Min_CheckID() != 0)
    {
        return 1;
    }

    // 1. 写基础初始化表。
    // 主要配置 OV5640 的时钟、ISP、DVP 输出等基础功能，让 OV5640 的时钟、ISP、DVP 等进入可工作状态。
    if (OV5640_Min_WriteTable(ov5640_init_reg_tbl,
                              sizeof(ov5640_init_reg_tbl) / sizeof(ov5640_init_reg_tbl[0])) != 0)
    {
        return 2;
    }
    // 等待基础配置稳定
    HAL_Delay(50);

    // 2.写 RGB565 模式表
    // 主要配置 RGB565 输出格式、PLL、PCLK、timing、ISP 等相关寄存器，确保 OV5640 输出 RGB565 格式的图像，并且时钟和时序满足 DCMI 的要求。
    if (OV5640_Min_WriteTable(ov5640_rgb565_reg_tbl,
                              sizeof(ov5640_rgb565_reg_tbl) / sizeof(ov5640_rgb565_reg_tbl[0])) != 0)
    {
        return 3;
    }
    // 等待 RGB565 配置稳定
    HAL_Delay(50);

    //3. 覆盖输出尺寸为 QVGA 320x240。
    // 注意：只改 DVP 输出尺寸，先不尝试完整手写裁剪/缩放表。

    // 设置 DVP 输出宽度为 320
    // 0x3808/0x3809 = 0x0140 = 320
    if (OV5640_Min_WriteReg(0x3808, 0x01)) return 4;  /* width  = 0x0140 = 320 */
    if (OV5640_Min_WriteReg(0x3809, 0x40)) return 5;

    // 设置 DVP 输出高度为 240
    // 0x380A/0x380B = 0x00F0 = 240
    if (OV5640_Min_WriteReg(0x380A, 0x00)) return 6;  /* height = 0x00F0 = 240 */
    if (OV5640_Min_WriteReg(0x380B, 0xF0)) return 7;

    // 4. 确保 DVP 输出格式是 RGB565。
    // 0x501F = 0x01 对应 RGB565 输出路径，如果之前的 RGB565 表没有覆盖这个寄存器，则在这里单独写入。
    if (OV5640_Min_WriteReg(0x501F, 0x01)) return 8;


    // 5.最后开启内部测试彩条
    if (OV5640_Min_EnableTestBar(1)) return 9;

    LOG_INFO("OV5640 full table RGB565 QVGA testbar init done");
    (void)OV5640_Min_ReadBackTimingDebug("QVGA_TESTBAR");

    return 0;
}

// 读回关键寄存器，用于确认配置是否真正写入 OV5640
uint8_t OV5640_Min_ReadBackDebug(void)
{
    uint8_t val = 0;

    // 测试彩条寄存器
    if (OV5640_Min_ReadReg(0x4741, &val)) return 1;
    LOG_INFO("OV5640 0x4741 = 0x%02X", val);

    // RGB/YUV/JPEG 输出格式控制寄存器
    if (OV5640_Min_ReadReg(0x4300, &val)) return 1;
    LOG_INFO("OV5640 0x4300 = 0x%02X", val);

    // ISP 输出格式选择寄存器
    if (OV5640_Min_ReadReg(0x501F, &val)) return 1;
    LOG_INFO("OV5640 0x501F = 0x%02X", val);

    // PLL 分频相关寄存器
    if (OV5640_Min_ReadReg(0x3035, &val)) return 1;
    LOG_INFO("OV5640 0x3035 = 0x%02X", val);

    // PLL 倍频相关寄存器
    if (OV5640_Min_ReadReg(0x3036, &val)) return 1;
    LOG_INFO("OV5640 0x3036 = 0x%02X", val);

    // PCLK 分频寄存器
    if (OV5640_Min_ReadReg(0x3824, &val)) return 1;
    LOG_INFO("OV5640 0x3824 = 0x%02X", val);

    // DVP 输出宽度高字节
    if (OV5640_Min_ReadReg(0x3808, &val)) return 1;
    LOG_INFO("OV5640 0x3808 = 0x%02X", val);

    // DVP 输出宽度低字节
    if (OV5640_Min_ReadReg(0x3809, &val)) return 1;
    LOG_INFO("OV5640 0x3809 = 0x%02X", val);

    // DVP 输出高度高字节
    if (OV5640_Min_ReadReg(0x380A, &val)) return 1;
    LOG_INFO("OV5640 0x380A = 0x%02X", val);

    // DVP 输出高度低字节
    if (OV5640_Min_ReadReg(0x380B, &val)) return 1;
    LOG_INFO("OV5640 0x380B = 0x%02X", val);

    return 0;
}

//回读 OV5640 时序相关的全部寄存器，用于详细调试与确认配置完整性
uint8_t OV5640_Min_ReadBackTimingDebug(const char *tag)
{
    // 需要回读的时序相关寄存器列表
    static const uint16_t regs[] =
    {
        0x3034, 0x3035, 0x3036, 0x3037,   // PLL 控制
        0x3108,                            // 系统时钟相关
        0x3800, 0x3801,                    // 输入窗口起始 X
        0x3802, 0x3803,                    // 输入窗口起始 Y
        0x3804, 0x3805,                    // 输入窗口结束 X
        0x3806, 0x3807,                    // 输入窗口结束 Y
        0x3808, 0x3809,                    // 输出宽度
        0x380A, 0x380B,                    // 输出高度
        0x380C, 0x380D,                    // 水平时序（HB/HS）
        0x380E, 0x380F,                    // 垂直时序（VB/VS）
        0x3810, 0x3811,                    // ISP X 偏移
        0x3812, 0x3813,                    // ISP Y 偏移
        0x3814, 0x3815,                    // 缩放/偏移配置
        0x3820, 0x3821,                    // 传感器/ISP 模式
        0x3824,                            // PCLK 分频
        0x4741,                            // 测试彩条
        0x5001                             // ISP 控制（裁剪/缩放使能）
    };
    uint8_t val = 0;

    LOG_INFO("OV5640 timing readback begin: %s", tag);

    // 遍历整个寄存器列表，逐一回读并输出
    for (uint32_t i = 0; i < (sizeof(regs) / sizeof(regs[0])); i++)
    {
        if (OV5640_Min_ReadReg(regs[i], &val))
        {
            LOG_ERROR("OV5640 timing readback failed: %s reg=0x%04X", tag, regs[i]);
            return 1;
        }

        LOG_INFO("OV5640 %s reg 0x%04X = 0x%02X", tag, regs[i], val);
    }

    LOG_INFO("OV5640 timing readback end: %s", tag);

    return 0;
}

//关闭彩条测试，输出传感器的分辨率 QVGA 的图像至LCD
uint8_t OV5640_Min_InitRGB565_QVGA_RealImage(void)
{
    uint8_t ret = 0;
    uint8_t val = 0;

   //先复用已经验证成功的完整 RGB565 + QVGA 初始化流程
    ret = OV5640_Min_InitRGB565_QVGA_TestBar();
    if (ret != 0)
    {
        LOG_ERROR("OV5640 RGB565 QVGA base init failed, ret = %d", ret);
        return ret;
    }

    //关闭 OV5640 测试图案，切换为真实图像输出
    ret = SCCB_WriteReg(0x4741, 0x00);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 disable test pattern failed");
        return 10;
    }

    HAL_Delay(20);

    //确认测试图案已经关闭
    ret = SCCB_ReadReg(0x4741, &val);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 read 0x4741 failed");
        return 11;
    }

    LOG_INFO("OV5640 0x4741 = 0x%02X", val);

    // 确保自动曝光/自动增益打开
    // 0x3503::
    //bit[0] AEC manual enable
    // bit[1] AGC manual enable
    // 写 0x00 表示让 AEC/AGC 自动工作。
    ret = SCCB_WriteReg(0x3503, 0x00);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 enable AEC/AGC failed");
        return 12;
    }

    // 等待自动曝光稳定几帧
    HAL_Delay(200);

    LOG_INFO("OV5640 RGB565 320x240 real image init done");

    return 0;
}

// 初始化 OV5640 为 RGB565 + 160x120 + 测试彩条
uint8_t OV5640_Min_InitRGB565_160x120_TestBar(void)
{
    // 先完成 RGB565 QVGA 测试彩条的基础初始化
    uint8_t ret = OV5640_Min_InitRGB565_QVGA_TestBar();

    if (ret != 0U)
    {
        return ret;
    }

    // 设置缩放后的输出尺寸为 160x120，并配置 ISP 偏移
    ret = OV5640_Min_OutSize_Set(4U, 0U, 160U, 120U);
    if (ret != 0U)
    {
        LOG_ERROR("OV5640 160x120 outsize set failed, ret = %d", ret);
        return 10U;
    }

    LOG_INFO("OV5640 RGB565 160x120 testbar init done");
    return 0U;
}

//关闭彩条测试，输出传感器的分辨率 160x120 的图像至LCD
uint8_t OV5640_Min_InitRGB565_160x120_RealImage(void)
{
    // 先完成 160x120 测试彩条的基础初始化（含尺寸缩小）
    uint8_t ret = OV5640_Min_InitRGB565_160x120_TestBar();

    if (ret != 0U)
    {
        return ret;
    }

    // 关闭测试彩条
    ret = OV5640_Min_EnableTestBar(0U);
    if (ret != 0U)
    {
        LOG_ERROR("OV5640 disable 160x120 test pattern failed");
        return 11U;
    }

    // 开启自动曝光/自动增益
    ret = OV5640_Min_WriteReg(0x3503U, 0x00U);
    if (ret != 0U)
    {
        LOG_ERROR("OV5640 enable 160x120 AEC/AGC failed");
        return 12U;
    }

    // 等待曝光稳定
    HAL_Delay(200U);
    LOG_INFO("OV5640 RGB565 160x120 real image init done");
    return 0U;
}

// 初始化 OV5640 为 RGB565 + 480x320 + 测试彩条
uint8_t OV5640_Min_InitRGB565_480x320_TestBar(void)
{
    // 先确认 SCCB 通信和芯片 ID 正常
    if (OV5640_Min_CheckID() != 0)
    {
        return 1;
    }

    // 1. 写基础初始化表（时钟、PLL、IO 等通用配置）
    if (OV5640_Min_WriteTable(ov5640_init_reg_tbl,
                              sizeof(ov5640_init_reg_tbl) / sizeof(ov5640_init_reg_tbl[0])) != 0)
    {
        return 2;
    }
    HAL_Delay(50);

    // 2. 写 RGB565 模式表（设置像素格式为 RGB565、关闭 JPEG 等）
    if (OV5640_Min_WriteTable(ov5640_rgb565_reg_tbl,
                              sizeof(ov5640_rgb565_reg_tbl) / sizeof(ov5640_rgb565_reg_tbl[0])) != 0)
    {
        return 3;
    }
    HAL_Delay(50);

    // 3. 保留 RGB565 表默认的完整图像窗口（约 2624x1706）
    //    通过 OV5640_Min_ImageWindow_Set 设置传感器/ISP 输入窗口起始和结束坐标
    if (OV5640_Min_ImageWindow_Set(0, 0, 0x0A40, 0x06AA)) return 4;

    // 4. 覆盖 DVP 输出尺寸为 480x320，ISP 会自动缩放到此尺寸
    if (OV5640_Min_OutSize_Set(4, 0, 480, 320)) return 5;

    // 5. 确保 DVP 输出格式是 RGB565
    if (OV5640_Min_WriteReg(0x501F, 0x01)) return 8;

    // 6. 开启内部测试彩条，便于检查数据通路和显示
    if (OV5640_Min_EnableTestBar(1)) return 9;

    LOG_INFO("OV5640 full table RGB565 480x320 testbar init done");
    // 回读关键时序寄存器，确认配置写入
    (void)OV5640_Min_ReadBackTimingDebug("480X320_TESTBAR");

    return 0;
}

//关闭彩条测试，输出传感器的分辨率 480x320 的图像至LCD
uint8_t OV5640_Min_InitRGB565_480x320_RealImage(void)
{
    uint8_t ret = 0;
    uint8_t val = 0;

    //复用 480x320 RGB565 测试彩条初始化流程，该流程已包含基础配置、RGB565模式、窗口、输出尺寸等
    ret = OV5640_Min_InitRGB565_480x320_TestBar();
    if (ret != 0)
    {
        LOG_ERROR("OV5640 RGB565 480x320 base init failed, ret = %d", ret);
        return ret;
    }

    // 再次设置输出尺寸为 480x320，防止测试彩条流程中可能被覆盖
    ret = OV5640_Min_OutSize_Set(4, 0, 480, 320);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 480x320 outsize set failed, ret = %d", ret);
        return 13;
    }

    // 关闭 OV5640 测试图案，切换为真实图像输出
    ret = SCCB_WriteReg(0x4741, 0x00);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 disable test pattern failed");
        return 10;
    }

    HAL_Delay(20);

    // 回读确认测试图案已经关闭
    ret = SCCB_ReadReg(0x4741, &val);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 read 0x4741 failed");
        return 11;
    }

    LOG_INFO("OV5640 0x4741 = 0x%02X", val);

    //确保自动曝光/自动增益打开
    //    寄存器 0x3503 bit[0]=AEC手动使能, bit[1]=AGC手动使能
    //    写 0x00 让 AEC/AGC 自动工作
    ret = SCCB_WriteReg(0x3503, 0x00);
    if (ret != 0)
    {
        LOG_ERROR("OV5640 enable AEC/AGC failed");
        return 12;
    }

    // 等待自动曝光稳定（几帧时间）
    HAL_Delay(200);

    LOG_INFO("OV5640 RGB565 480x320 real image init done");

    return 0;
}
