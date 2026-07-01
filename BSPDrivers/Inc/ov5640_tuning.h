#ifndef ISP_OV5640_TUNING_H
#define ISP_OV5640_TUNING_H

#include <stdint.h>

uint8_t OV5640_Tuning_DumpAECRegs(void);
uint8_t OV5640_Tuning_GetExposureRaw(uint32_t *exposure_raw);
uint8_t OV5640_Tuning_GetGainRaw(uint16_t *gain_raw);

#endif /* ISP_OV5640_TUNING_H */
