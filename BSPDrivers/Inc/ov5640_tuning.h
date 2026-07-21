#ifndef ISP_OV5640_TUNING_H
#define ISP_OV5640_TUNING_H

#include <stdint.h>

typedef enum
{
    OV5640_AEC_TARGET_BASELINE = 0,
    OV5640_AEC_TARGET_MINUS_1,
    OV5640_AEC_TARGET_MINUS_2,
} OV5640_AecTargetLevel_t;

typedef enum
{
    OV5640_AWB_MODE_AUTO = 0,
    OV5640_AWB_MODE_SUNNY,
    OV5640_AWB_MODE_CLOUDY,
    OV5640_AWB_MODE_OFFICE,
    OV5640_AWB_MODE_HOME,
} OV5640_AwbMode_t;

typedef enum
{
    OV5640_IMAGE_PARAM_DEFAULT = 0,
    OV5640_IMAGE_PARAM_MINUS_1 = -1,
    OV5640_IMAGE_PARAM_PLUS_1  = 1,
} OV5640_ImageParamLevel_t;

uint8_t OV5640_Tuning_DumpAECRegs(void);
uint8_t OV5640_Tuning_GetExposureRaw(uint32_t *exposure_raw);
uint8_t OV5640_Tuning_GetGainRaw(uint16_t *gain_raw);
uint8_t OV5640_Tuning_SetAecTarget(OV5640_AecTargetLevel_t level);
uint8_t OV5640_Tuning_SetAWBMode(OV5640_AwbMode_t mode);
void OV5640_Tuning_DumpAWBRegs(void);
uint8_t OV5640_Tuning_SetBrightness(int8_t level);
uint8_t OV5640_Tuning_SetContrast(int8_t level);
uint8_t OV5640_Tuning_SetSaturation(int8_t level);
uint8_t OV5640_Tuning_SetSharpness(uint8_t level);

#endif /* ISP_OV5640_TUNING_H */
