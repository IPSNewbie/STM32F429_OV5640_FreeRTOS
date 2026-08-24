//
// Created by FAKE on 2026/6/3.
//

#ifndef ISP_OV5640_OV5640CFG_H
#define ISP_OV5640_OV5640CFG_H
// #include "ov5640.h"

/**
 * @file OV5640cfg.h
 * @brief OV5640 厂家参考寄存器配置表和项目已验证的 RGB565 覆盖表
 *
 * 每个表项按“16 位寄存器地址、低 8 位寄存器值”成对保存，并由
 * OV5640_Min_WriteTable() 严格按顺序写入。表内包含大量模拟阵列、ISP 和
 * 时钟相关的厂家参考值；对没有本地数据手册证据的 0x36xx/0x37xx/0x39xx
 * 等寄存器只按功能组说明，不推测单个位的含义。
 *
 * @note 表项顺序、寄存器地址和值属于已通过硬件验证的初始化时序。
 *       0x3008=0x42 使传感器进入 software power-down，表尾 0x3008=0x02
 *       才将其唤醒；两者共同构成初始化过程，不能只保留其中一项。
 */

/** @brief JPEG 输出参考配置表，最大支持 2592x1944。 */
const uint16_t OV5640_jpeg_reg_tbl[][2] =
{
    // 下面这组用于 JPEG 模式，当前 RGB565 调试阶段暂时不用
    // 4300/501F 决定输出数据格式，这里配置为 YUV/JPEG 相关路径
    0x4300, 0x30, /* YUV 422, YUYV */
    0x501f, 0x00, /* YUV 422 */

    /* Input clock = 24Mhz */
    // PLL 配置，决定内部时钟和输出像素时钟基础频率
    0x3035, 0x21, /* PLL */
    0x3036, 0x69, /* PLL */

    0x3c07, 0x07, /* lightmeter 1 threshold[7:0] */

    // 图像翻转/镜像控制
    0x3820, 0x46, /* flip */
    0x3821, 0x20, /* mirror */

    // X/Y 采样步进，影响缩放/抽样方式
    0x3814, 0x11, /* timing X inc */
    0x3815, 0x11, /* timing Y inc */

    // 输入窗口起点 HS/VS
    0x3800, 0x00, /* HS */
    0x3801, 0x00, /* HS */
    0x3802, 0x00, /* VS */
    0x3803, 0x00, /* VS */

    // 输入窗口终点 HE/VE
    0x3804, 0x0a, /* HW (HE) */
    0x3805, 0x3f, /* HW (HE) */
    0x3806, 0x07, /* VH (VE) */
    0x3807, 0x9f, /* VH (VE) */

    // DVP 输出图像宽度，0x0280 = 640
    0x3808, 0x02, /* DVPHO */
    0x3809, 0x80, /* DVPHO */

    // DVP 输出图像高度，0x01E0 = 480
    0x380a, 0x01, /* DVPVO */
    0x380b, 0xe0, /* DVPVO */

    // HTS：一行总时钟数，影响行周期/PCLK节奏
    0x380c, 0x0b, /* HTS */
    0x380d, 0x1c, /* HTS */

    // VTS：一帧总行数，影响帧率
    0x380e, 0x07, /* VTS */
    0x380f, 0xb0, /* VTS */

    0x3813, 0x04, /* timing V offset   04 */

    // 模拟/传感器阵列相关参数，通常不单独改变，保持手册推荐值
    0x3618, 0x04,
    0x3612, 0x2b,
    0x3709, 0x12,
    0x370c, 0x00,

    0x4004, 0x06, /* BLC line number */

    // JPEG FIFO/JPEG时钟相关配置
    0x3002, 0x00, /* enable JFIFO, SFIFO, JPG */
    0x3006, 0xff, /* enable clock of JPEG2x, JPEG */
    0x4713, 0x03, /* JPEG mode 3 */
    0x4407, 0x01, /* Quantization sacle */
    0x460b, 0x35,
    0x460c, 0x22,

    // PCLK/MIPI时序相关，DVP模式下部分仍会影响输出时序
    0x4837, 0x16, /* 厂家参考时序值；当前不对该寄存器做位级推断 */
    0x3824, 0x02, /* PCLK manual divider */

    // ISP功能开关：缩放、色彩矩阵、AWB等
    0x5001, 0xA3, /* SDE on, Scaling on, CMX on, AWB on */

    // 自动曝光/自动增益开启
    0x3503, 0x00, /* AEC/AGC on */
};

/** @brief RGB565 输出参考配置表，最大支持 1280x800。 */
const uint16_t ov5640_rgb565_reg_tbl[][2] =
{
    // 4300 控制输出格式，这里配置为 RGB565 相关输出
    // 正点原子这里用 0x6F，先不要随意改成 0x61
    0x4300, 0X6F,

    // 501F 选择 ISP 输出格式路径，0x01 对应 RGB565
    0X501F, 0x01,

    /* 1280x800, 15fps */
    /* input clock 24Mhz, PCLK 42Mhz */
    //注意，像素时钟不能超过DCMI的时钟HCLK的1/4，本项目中HCLK = 180Mhz，即PCLK <= 45Mhz

    // PLL 配置，决定内部时钟和 PCLK
    // 3035/3036 是 OV5640 出图的关键寄存器，不能省
    0x3035, 0x41, /* PLL */
    0x3036, 0x69, /* PLL */

    0x3c07, 0x07, /* lightmeter 1 threshold[7:0] */

    // 图像翻转/镜像
    0x3820, 0x46, /* flip */
    0x3821, 0x00, /* mirror */

    // X/Y 采样步进，0x31 表示缩放/抽样模式下的步进设置
    0x3814, 0x31, /* timing X inc */
    0x3815, 0x31, /* timing Y inc */

    // 输入传感器窗口起点
    //X_start = 0x0000 = 0
    //Y_start = 0x0000 = 0
    0x3800, 0x00, /* HS */
    0x3801, 0x00, /* HS */
    0x3802, 0x00, /* VS */
    0x3803, 0x00, /* VS */

    // 输入传感器窗口终点，决定从 sensor 原始阵列中取多大区域
    //X_end   = 0x0A3F = 2623
    //Y_end   = 0x06A9 = 1705
    0x3804, 0x0a, /* HW (HE) */
    0x3805, 0x3f, /* HW (HE) */
    0x3806, 0x06, /* VH (VE) */
    0x3807, 0xa9, /* VH (VE) */

    // DVP 输出宽度，0x0500 = 1280
    // 后面我们会在 ov5640.c 中覆盖为 320
    0x3808, 0x05, /* DVPHO */
    0x3809, 0x00, /* DVPHO */

    // DVP 输出高度，0x02D0 = 720
    // 后面我们会在 ov5640.c 中覆盖为 240
    0x380a, 0x02, /* DVPVO */
    0x380b, 0xd0, /* DVPVO */

    // HTS：一行总周期，影响 PCLK 下的行输出节奏
    0x380c, 0x05, /* HTS */
    0x380d, 0xF8, /* HTS */

    // VTS：一帧总行数，影响帧率
    0x380e, 0x03, /* VTS */
    0x380f, 0x84, /* VTS */

    0x3813, 0x04, /* timing V offset */

    // 模拟阵列/时序相关寄存器，保持原表配置
    0x3618, 0x00,
    0x3612, 0x29,
    0x3709, 0x52,
    0x370c, 0x03,

    // 自动曝光最大曝光行数限制，影响亮度和帧率稳定性
    0x3a02, 0x02, /* 60Hz max exposure */
    0x3a03, 0xe0, /* 60Hz max exposure */

    0x3a14, 0x02, /* 50Hz max exposure */
    0x3a15, 0xe0, /* 50Hz max exposure */

    0x4004, 0x02, /* BLC line number */

    // RGB565 模式下关闭/复位 JPEG FIFO 相关功能
    0x3002, 0x1c, /* reset JFIFO, SFIFO, JPG */
    0x3006, 0xc3, /* disable clock of JPEG2x, JPEG */

    // JPEG相关寄存器保留原配置，对RGB565调试影响不大
    0x4713, 0x03, /* JPEG mode 3 */
    0x4407, 0x04, /* Quantization scale */
    0x460b, 0x37,
    0x460c, 0x20,

    // 时序相关
    0x4837, 0x16, /* 厂家参考时序值；当前不对该寄存器做位级推断 */

    // PCLK手动分频，影响 DCMI 接收数据速度
    0x3824, 0x04, /* PCLK manual divider */

    // ISP功能开关：SDE、缩放、色彩矩阵、AWB
    0x5001, 0xA3, /* SDE on, scale on, UV average off, color matrix on, AWB on */

    // AEC/AGC自动曝光/自动增益开启
    0x3503, 0x00, /* AEC/AGC on */
};

/** @brief OV5640 UXGA 基础初始化寄存器序列表。 */
const uint16_t ov5640_init_reg_tbl[][2] =
{
    /* 24MHz input clock, 24MHz PCLK */

    // 退出软件掉电，bit[6] 控制 software power down。此模式与PWDN不同，PWDN硬件掉电，传感器进入低功耗，SCCB 通常不能正常访问；
    // 而软件掉电则是通过寄存器控制，SCCB 仍可访问寄存器，适合在运行时临时降低功耗。
    0x3008, 0x42, /* software power down, bit[6] */

    // 系统时钟来源配置
    0x3103, 0x03, /* system clock from PLL, bit[1] */

    // DVP输出引脚使能：FREX/VSYNC/HREF/PCLK/D[9:6]
    0x3017, 0xff, /* FREX, Vsync, HREF, PCLK, D[9:6] output enable */

    // DVP输出引脚使能：D[5:0]/GPIO
    /*
     * 0x3018 控制 DVP 数据输出使能。项目经硬件验证确认 bit[6:4] 对应
     * D4/D3/D2；SD SNAPSHOT 写卡时使用 value & 0x8F 临时关闭这三路，
     * 因为 D2/D3/D4 分别与 SDIO 的 PC8/PC9/PC11 复用，cleanup 后恢复原值。
     */
    0x3018, 0xff, /* 基础初始化阶段保持完整 DVP 数据输出 */

    // MIPI数据位宽相关配置，虽然我们用DVP，仍保留原表
    0x3034, 0x1a, /* MIPI 10-bit */

    // PLL根分频/预分频配置
    0x3037, 0x13, /* PLL root divider, bit[4], PLL pre-divider, bit[3:0] */

    // PCLK/SCLK分频配置
    0x3108, 0x01, /* PCLK root divider, bit[5:4], SCLK2x root divider, bit[3:2] */

    /* SCLK root divider, bit[1:0] */

    // 以下 0x36xx/0x37xx/0x39xx 多为模拟前端、阵列时序、稳定性配置
    // 一般不单独修改，直接使用成熟表
    0x3630, 0x36,
    0x3631, 0x0e,
    0x3632, 0xe2,
    0x3633, 0x12,
    0x3621, 0xe0,
    0x3704, 0xa0,
    0x3703, 0x5a,
    0x3715, 0x78,
    0x3717, 0x01,
    0x370b, 0x60,
    0x3705, 0x1a,
    0x3905, 0x02,
    0x3906, 0x10,
    0x3901, 0x0a,
    0x3731, 0x12,

    // VCM 镜头控制相关
    0x3600, 0x08, /* VCM control */
    0x3601, 0x33, /* VCM control */

    0x302d, 0x60, /* system control */
    0x3620, 0x52,
    0x371b, 0x20,
    0x471c, 0x50,

    // 自动曝光/增益相关
    0x3a13, 0x43, /* pre-gain = 1.047x */
    0x3a18, 0x00, /* gain ceiling */
    0x3a19, 0xf8, /* gain ceiling = 15.5x */

    0x3635, 0x13,
    0x3636, 0x03,
    0x3634, 0x40,
    0x3622, 0x01,

    /* 50/60Hz detection 50/60Hz 灯光条纹过滤 */
    // 防止室内灯光导致横纹闪烁
    0x3c01, 0x34, /* Band auto, bit[7] */
    0x3c04, 0x28, /* threshold low sum */
    0x3c05, 0x98, /* threshold high sum */
    0x3c06, 0x00, /* light meter 1 threshold[15:8] */
    0x3c07, 0x08, /* light meter 1 threshold[7:0] */
    0x3c08, 0x00, /* light meter 2 threshold[15:8] */
    0x3c09, 0x1c, /* light meter 2 threshold[7:0] */
    0x3c0a, 0x9c, /* sample number[15:8] */
    0x3c0b, 0x40, /* sample number[7:0] */

    // 图像输出偏移
    0x3810, 0x00, /* Timing Hoffset[11:8] */
    0x3811, 0x10, /* Timing Hoffset[7:0] */
    0x3812, 0x00, /* Timing Voffset[10:8] */

    0x3708, 0x64,

    // BLC 黑电平校正
    0x4001, 0x02, /* BLC start from line 2 */
    0x4005, 0x1a, /* BLC always update */

    // 模块使能和时钟使能
    0x3000, 0x00, /* enable blocks */
    0x3004, 0xff, /* enable clocks */

    // 关键：MIPI power down，DVP enable
    // 这里保证使用 DVP 并口输出，而不是 MIPI
    0x300e, 0x58, /* MIPI power down, DVP enable */

    0x302e, 0x00,

    // 初始表默认先配置为 YUV，后续 RGB565表会覆盖
    0x4300, 0x30, /* YUV 422, YUYV */
    0x501f, 0x00, /* YUV 422 */

    0x440e, 0x00,

    // ISP模块使能：镜头校正、Gamma、坏点校正等
    0x5000, 0xa7, /* Lenc on, raw gamma on, BPC on, WPC on, CIP on */

    /* AEC target 自动曝光控制 */
    // 自动曝光目标范围，影响画面整体亮度稳定
    0x3a0f, 0x30, /* stable range in high */
    0x3a10, 0x28, /* stable range in low */
    0x3a1b, 0x30, /* stable range out high */
    0x3a1e, 0x26, /* stable range out low */
    0x3a11, 0x60, /* fast zone high */
    0x3a1f, 0x14, /* fast zone low */

    /* Lens correction for ? 镜头补偿 */
    // 0x58xx 是镜头阴影/边缘亮度补偿表
    0x5800, 0x23,
    0x5801, 0x14,
    0x5802, 0x0f,
    0x5803, 0x0f,
    0x5804, 0x12,
    0x5805, 0x26,
    0x5806, 0x0c,
    0x5807, 0x08,
    0x5808, 0x05,
    0x5809, 0x05,
    0x580a, 0x08,

    0x580b, 0x0d,
    0x580c, 0x08,
    0x580d, 0x03,
    0x580e, 0x00,
    0x580f, 0x00,
    0x5810, 0x03,
    0x5811, 0x09,
    0x5812, 0x07,
    0x5813, 0x03,
    0x5814, 0x00,
    0x5815, 0x01,
    0x5816, 0x03,
    0x5817, 0x08,
    0x5818, 0x0d,
    0x5819, 0x08,
    0x581a, 0x05,
    0x581b, 0x06,
    0x581c, 0x08,
    0x581d, 0x0e,
    0x581e, 0x29,
    0x581f, 0x17,
    0x5820, 0x11,
    0x5821, 0x11,
    0x5822, 0x15,
    0x5823, 0x28,
    0x5824, 0x46,
    0x5825, 0x26,
    0x5826, 0x08,
    0x5827, 0x26,
    0x5828, 0x64,
    0x5829, 0x26,
    0x582a, 0x24,
    0x582b, 0x22,
    0x582c, 0x24,
    0x582d, 0x24,
    0x582e, 0x06,
    0x582f, 0x22,
    0x5830, 0x40,
    0x5831, 0x42,
    0x5832, 0x24,
    0x5833, 0x26,
    0x5834, 0x24,
    0x5835, 0x22,
    0x5836, 0x22,
    0x5837, 0x26,
    0x5838, 0x44,
    0x5839, 0x24,
    0x583a, 0x26,
    0x583b, 0x28,
    0x583c, 0x42,
    0x583d, 0xce, /* lenc BR offset */

    /* AWB 自动白平衡 */
    // 0x5180~0x519E 是自动白平衡参数表
    // 影响不同光源下的红/绿/蓝增益
    0x5180, 0xff, /* AWB B block */
    0x5181, 0xf2, /* AWB control */
    0x5182, 0x00, /* [7:4] max local counter, [3:0] max fast counter */
    0x5183, 0x14, /* AWB advanced */
    0x5184, 0x25,
    0x5185, 0x24,
    0x5186, 0x09,
    0x5187, 0x09,
    0x5188, 0x09,
    0x5189, 0x75,
    0x518a, 0x54,
    0x518b, 0xe0,
    0x518c, 0xb2,
    0x518d, 0x42,
    0x518e, 0x3d,
    0x518f, 0x56,
    0x5190, 0x46,
    0x5191, 0xf8, /* AWB top limit */
    0x5192, 0x04, /* AWB bottom limit */
    0x5193, 0x70, /* red limit */
    0x5194, 0xf0, /* green limit */
    0x5195, 0xf0, /* blue limit */
    0x5196, 0x03, /* AWB control */
    0x5197, 0x01, /* local limit */
    0x5198, 0x04,
    0x5199, 0x12,
    0x519a, 0x04,
    0x519b, 0x00,
    0x519c, 0x06,
    0x519d, 0x82,
    0x519e, 0x38, /* AWB control */

    /* Gamma 伽玛曲线 */
    // 0x5480~0x5490 控制亮度曲线，影响暗部/亮部过渡
    0x5480, 0x01, /* Gamma bias plus on, bit[0] */
    0x5481, 0x08,
    0x5482, 0x14,
    0x5483, 0x28,
    0x5484, 0x51,
    0x5485, 0x65,
    0x5486, 0x71,
    0x5487, 0x7d,
    0x5488, 0x87,
    0x5489, 0x91,
    0x548a, 0x9a,
    0x548b, 0xaa,
    0x548c, 0xb8,
    0x548d, 0xcd,
    0x548e, 0xdd,
    0x548f, 0xea,
    0x5490, 0x1d,

    /* color matrix 色彩矩阵 */
    // 0x5381~0x538B 是色彩矩阵，影响颜色还原和色偏
    0x5381, 0x1e, /* CMX1 for Y */
    0x5382, 0x5b, /* CMX2 for Y */
    0x5383, 0x08, /* CMX3 for Y */
    0x5384, 0x0a, /* CMX4 for U */
    0x5385, 0x7e, /* CMX5 for U */
    0x5386, 0x88, /* CMX6 for U */
    0x5387, 0x7c, /* CMX7 for V */
    0x5388, 0x6c, /* CMX8 for V */
    0x5389, 0x10, /* CMX9 for V */
    0x538a, 0x01, /* sign[9] */
    0x538b, 0x98, /* sign[8:1] */

    /* UV adjust UV 色彩饱和度调整 */
    // 饱和度、色度相关配置
    0x5580, 0x06, /* saturation on, bit[1] */
    0x5583, 0x40,
    0x5584, 0x10,
    0x5589, 0x10,
    0x558a, 0x00,
    0x558b, 0xf8,

    // 手动对比度偏移使能
    0x501d, 0x40, /* enable manual offset of contrast */

    /* CIP 锐化和降噪 */
    // 0x5300~0x530C 控制锐化、降噪阈值
    0x5300, 0x08, /* CIP sharpen MT threshold 1 */
    0x5301, 0x30, /* CIP sharpen MT threshold 2 */
    0x5302, 0x10, /* CIP sharpen MT offset 1 */
    0x5303, 0x00, /* CIP sharpen MT offset 2 */
    0x5304, 0x08, /* CIP DNS threshold 1 */
    0x5305, 0x30, /* CIP DNS threshold 2 */
    0x5306, 0x08, /* CIP DNS offset 1 */
    0x5307, 0x16, /* CIP DNS offset 2 */
    0x5309, 0x08, /* CIP sharpen TH threshold 1 */
    0x530a, 0x30, /* CIP sharpen TH threshold 2 */
    0x530b, 0x04, /* CIP sharpen TH offset 1 */
    0x530c, 0x06, /* CIP sharpen TH offset 2 */

    0x5025, 0x00,

    // 从 standby 唤醒，开始正常工作
    0x3008, 0x02, /* wake up from standby, bit[6] */

    /*自行添加的设置 */
    // VSYNC 极性设置
    // 你当前 DCMI 配置为 VSYNC 低有效，若画面异常可重点检查这里和 DCMI 极性是否匹配
    0x4740, 0X21, /* 与当前 DCMI 配置配套且已硬件验证；不在此推断极性位语义 */
};

#endif //ISP_OV5640_OV5640CFG_H
