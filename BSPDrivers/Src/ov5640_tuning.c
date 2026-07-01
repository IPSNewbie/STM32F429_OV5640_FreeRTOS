#include "ov5640_tuning.h"
#include "bsp_log.h"
#include "bsp_sccb.h"

typedef struct
{
    uint16_t reg;
    const char *name;
} OV5640_Tuning_RegDesc;

typedef struct
{
    uint8_t aec_wpt;
    uint8_t aec_bpt;
    uint8_t aec_wpt2;
    uint8_t aec_bpt2;
} OV5640_Tuning_AecTargetCfg;

enum
{
    AEC_REG_EXPOSURE_H = 0,
    AEC_REG_EXPOSURE_M,
    AEC_REG_EXPOSURE_L,
    AEC_REG_AEC_AGC_CTRL,
    AEC_REG_GAIN_H,
    AEC_REG_GAIN_L,
    AEC_REG_AEC_CTRL_00,
    AEC_REG_AEC_WPT,
    AEC_REG_AEC_BPT,
    AEC_REG_AEC_WPT2,
    AEC_REG_AEC_BPT2,
    AEC_REG_GAIN_CEIL_H,
    AEC_REG_GAIN_CEIL_L,
    AEC_REG_COUNT
};

static const OV5640_Tuning_RegDesc s_aec_regs[AEC_REG_COUNT] =
{
    {0x3500U, "EXPOSURE_H  "},
    {0x3501U, "EXPOSURE_M  "},
    {0x3502U, "EXPOSURE_L  "},
    {0x3503U, "AEC_AGC_CTRL"},
    {0x350AU, "GAIN_H      "},
    {0x350BU, "GAIN_L      "},
    {0x3A00U, "AEC_CTRL_00 "},
    {0x3A0FU, "AEC_WPT     "},
    {0x3A10U, "AEC_BPT     "},
    {0x3A1BU, "AEC_WPT2    "},
    {0x3A1EU, "AEC_BPT2    "},
    {0x3A18U, "GAIN_CEIL_H "},
    {0x3A19U, "GAIN_CEIL_L "}
};

static const OV5640_Tuning_AecTargetCfg s_aec_target_cfg[] =
{
    {0x30U, 0x28U, 0x30U, 0x26U},
    {0x2CU, 0x24U, 0x2CU, 0x22U},
    {0x28U, 0x20U, 0x28U, 0x1EU}
};

static uint32_t OV5640_Tuning_ComposeExposureRaw(uint8_t high,
                                                 uint8_t middle,
                                                 uint8_t low)
{
    return (((uint32_t)(high & 0x0FU)) << 16) |
           (((uint32_t)middle) << 8) |
           ((uint32_t)low);
}

static uint16_t OV5640_Tuning_ComposeGainRaw(uint8_t high, uint8_t low)
{
    return (uint16_t)((((uint16_t)(high & 0x03U)) << 8) | low);
}

static void OV5640_Tuning_PrintReg(uint32_t index, const uint8_t *values)
{
    LOG_RAW("%s 0x%04X = 0x%02X\r\n",
            s_aec_regs[index].name,
            (unsigned int)s_aec_regs[index].reg,
            (unsigned int)values[index]);
}

uint8_t OV5640_Tuning_GetExposureRaw(uint32_t *exposure_raw)
{
    uint8_t exposure_h;
    uint8_t exposure_m;
    uint8_t exposure_l;

    if (exposure_raw == 0)
    {
        return 1U;
    }

    if (SCCB_ReadReg(0x3500U, &exposure_h) != 0U) return 2U;
    if (SCCB_ReadReg(0x3501U, &exposure_m) != 0U) return 3U;
    if (SCCB_ReadReg(0x3502U, &exposure_l) != 0U) return 4U;

    *exposure_raw = OV5640_Tuning_ComposeExposureRaw(exposure_h,
                                                     exposure_m,
                                                     exposure_l);
    return 0U;
}

uint8_t OV5640_Tuning_GetGainRaw(uint16_t *gain_raw)
{
    uint8_t gain_h;
    uint8_t gain_l;

    if (gain_raw == 0)
    {
        return 1U;
    }

    if (SCCB_ReadReg(0x350AU, &gain_h) != 0U) return 2U;
    if (SCCB_ReadReg(0x350BU, &gain_l) != 0U) return 3U;

    *gain_raw = OV5640_Tuning_ComposeGainRaw(gain_h, gain_l);
    return 0U;
}

uint8_t OV5640_Tuning_DumpAECRegs(void)
{
    uint8_t values[AEC_REG_COUNT];
    uint32_t exposure_raw;
    uint16_t gain_raw;

    for (uint32_t i = 0U; i < AEC_REG_COUNT; ++i)
    {
        if (SCCB_ReadReg(s_aec_regs[i].reg, &values[i]) != 0U)
        {
            LOG_RAW("OV5640 AEC/AGC dump read failed: 0x%04X\r\n",
                    (unsigned int)s_aec_regs[i].reg);
            return (uint8_t)(i + 1U);
        }
    }

    exposure_raw = OV5640_Tuning_ComposeExposureRaw(values[AEC_REG_EXPOSURE_H],
                                                    values[AEC_REG_EXPOSURE_M],
                                                    values[AEC_REG_EXPOSURE_L]);
    gain_raw = OV5640_Tuning_ComposeGainRaw(values[AEC_REG_GAIN_H],
                                            values[AEC_REG_GAIN_L]);

    LOG_RAW("========== OV5640 AEC/AGC DUMP ==========\r\n");
    OV5640_Tuning_PrintReg(AEC_REG_EXPOSURE_H, values);
    OV5640_Tuning_PrintReg(AEC_REG_EXPOSURE_M, values);
    OV5640_Tuning_PrintReg(AEC_REG_EXPOSURE_L, values);
    LOG_RAW("Exposure raw = 0x%05lX\r\n", (unsigned long)exposure_raw);
    OV5640_Tuning_PrintReg(AEC_REG_AEC_AGC_CTRL, values);
    OV5640_Tuning_PrintReg(AEC_REG_GAIN_H, values);
    OV5640_Tuning_PrintReg(AEC_REG_GAIN_L, values);
    LOG_RAW("Gain raw     = 0x%03X\r\n", (unsigned int)gain_raw);

    for (uint32_t i = AEC_REG_AEC_CTRL_00; i < AEC_REG_COUNT; ++i)
    {
        OV5640_Tuning_PrintReg(i, values);
    }

    LOG_RAW("=========================================\r\n");
    return 0U;
}

uint8_t OV5640_Tuning_SetAecTarget(OV5640_AecTargetLevel_t level)
{
    const OV5640_Tuning_AecTargetCfg *cfg;

    if ((uint32_t)level >= (sizeof(s_aec_target_cfg) / sizeof(s_aec_target_cfg[0])))
    {
        return 1U;
    }

    cfg = &s_aec_target_cfg[(uint32_t)level];

    if (SCCB_WriteReg(0x3A0FU, cfg->aec_wpt) != 0U) return 2U;
    if (SCCB_WriteReg(0x3A10U, cfg->aec_bpt) != 0U) return 3U;
    if (SCCB_WriteReg(0x3A1BU, cfg->aec_wpt2) != 0U) return 4U;
    if (SCCB_WriteReg(0x3A1EU, cfg->aec_bpt2) != 0U) return 5U;

    return OV5640_Tuning_DumpAECRegs();
}
