#ifndef ISP_OV5640_TUNING_H
#define ISP_OV5640_TUNING_H

#include <stdint.h> // 提供原始曝光、增益、档位和返回码的固定宽度类型

/**
 * @file ov5640_tuning.h
 * @brief OV5640 AEC/AWB 与基础画质离散档位调试接口
 *
 * 本接口只改变调用者明确选择的寄存器组，不负责采集图像或评价图像质量。
 * 调参应一次只改变一类参数，再通过 PC Dump 在相同场景下比较量化指标。
 * 所有调用都通过 SCCB 同步访问传感器，限任务上下文使用，不支持并发调用。
 */

/**
 * @brief 自动曝光目标亮度档位
 */
typedef enum
{
    OV5640_AEC_TARGET_BASELINE = 0, /**< 当前验证基线 */
    OV5640_AEC_TARGET_MINUS_1,      /**< 比基线降低一档 */
    OV5640_AEC_TARGET_MINUS_2,      /**< 比基线降低两档 */
} OV5640_AecTargetLevel_t;

/**
 * @brief 自动白平衡及预设白平衡模式
 */
typedef enum
{
    OV5640_AWB_MODE_AUTO = 0, /**< 自动白平衡 */
    OV5640_AWB_MODE_SUNNY,    /**< 晴天预设 */
    OV5640_AWB_MODE_CLOUDY,   /**< 阴天预设 */
    OV5640_AWB_MODE_OFFICE,   /**< 办公室照明预设 */
    OV5640_AWB_MODE_HOME,     /**< 家庭照明预设 */
} OV5640_AwbMode_t;

/**
 * @brief 亮度、对比度和饱和度的统一调节档位
 */
typedef enum
{
    OV5640_IMAGE_PARAM_DEFAULT = 0,  /**< 默认值 */
    OV5640_IMAGE_PARAM_MINUS_1 = -1, /**< 降低一档 */
    OV5640_IMAGE_PARAM_PLUS_1  = 1,  /**< 提高一档 */
} OV5640_ImageParamLevel_t;

/**
 * @brief 读取并输出当前 AEC/AGC 关键寄存器
 * @return 0-全部读取成功，1~13-对应表项读取失败
 * @note 组合输出包含 20 位原始曝光值和 10 位原始增益值；不修改寄存器。
 */
uint8_t OV5640_Tuning_DumpAECRegs(void);

/**
 * @brief 读取 20 位曝光寄存器原始值
 * @param exposure_raw 接收曝光原始值的输出指针
 * @return 0-成功，1-参数非法，2~4-对应寄存器读取失败
 * @note 返回的是寄存器拼接后的原始量，不直接等同于微秒曝光时间。
 */
uint8_t OV5640_Tuning_GetExposureRaw(uint32_t *exposure_raw);

/**
 * @brief 读取自动增益寄存器原始值
 * @param gain_raw 接收增益原始值的输出指针
 * @return 0-成功，1-参数非法，2~3-对应寄存器读取失败
 * @note 返回的是 0x350A/0x350B 的 10 位原始编码，不换算为实际倍数。
 */
uint8_t OV5640_Tuning_GetGainRaw(uint16_t *gain_raw);

/**
 * @brief 设置自动曝光目标亮度档位
 * @param level 目标档位
 * @return 0-成功，非 0-档位非法、寄存器写入或回读失败
 * @note 只调整 AEC 目标阈值，不手动写 0x3500~0x3502 曝光值。
 */
uint8_t OV5640_Tuning_SetAecTarget(OV5640_AecTargetLevel_t level);

/**
 * @brief 设置自动或预设白平衡模式
 * @param mode 白平衡模式
 * @return 0-成功，非 0-模式非法或寄存器写入失败
 * @note 手动预设是待 PC Dump 验证的起点，不代表所有光源下的最终标定值。
 */
uint8_t OV5640_Tuning_SetAWBMode(OV5640_AwbMode_t mode);

/** @brief 输出 0x3400~0x3406 当前值；单个读取失败会记录后继续。 */
void OV5640_Tuning_DumpAWBRegs(void);

/** @brief 设置亮度档位。 @param level -1、0 或 1 @return 0-成功，1-参数非法，2~4-寄存器写入失败 */
uint8_t OV5640_Tuning_SetBrightness(int8_t level);

/** @brief 设置对比度档位。 @param level -1、0 或 1 @return 0-成功，1-参数非法，2~5-寄存器写入失败 */
uint8_t OV5640_Tuning_SetContrast(int8_t level);

/** @brief 设置饱和度档位。 @param level 0、1 或 2 @return 0-成功，1-参数非法，2~4-寄存器写入失败 */
uint8_t OV5640_Tuning_SetSaturation(int8_t level);

/**
 * @brief 设置锐度等级
 * @param level 锐度等级，范围 0~2
 * @return 0-成功，1-参数非法，2~9-对应寄存器写入失败
 */
uint8_t OV5640_Tuning_SetSharpness(uint8_t level);

#endif /* ISP_OV5640_TUNING_H */
