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

typedef struct
{
    uint16_t r_gain;
    uint16_t g_gain;
    uint16_t b_gain;
} OV5640_Tuning_AwbGainCfg;

typedef struct
{
    uint8_t offset;
    uint8_t sign;
} OV5640_Tuning_BrightnessCfg;

typedef struct
{
    uint8_t center;
    uint8_t gain;
} OV5640_Tuning_ContrastCfg;

typedef struct
{
    uint8_t u_gain;
    uint8_t v_gain;
} OV5640_Tuning_SaturationCfg;

typedef struct
{
    uint8_t mt_th1;
    uint8_t mt_th2;
    uint8_t mt_offset1;
    uint8_t mt_offset2;
    uint8_t th_th1;
    uint8_t th_th2;
    uint8_t th_offset1;
    uint8_t th_offset2;
} OV5640_Tuning_SharpnessCfg;

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

/*
 * Manual AWB gain presets for OV5640 0x3400~0x3406.
 * These are initial light-mode gain presets and must be validated by PC Dump.
 */
static const OV5640_Tuning_AwbGainCfg s_awb_manual_gain_cfg[] =
{
    {0x061CU, 0x0400U, 0x04F3U},  /* sunny */
    {0x0648U, 0x0400U, 0x04D3U},  /* cloudy */
    {0x0548U, 0x0400U, 0x07CFU},  /* office */
    {0x0410U, 0x0400U, 0x0840U}   /* home */
};

/*
 * Initial SDE/CIP presets. No exact project-specific tuning table exists yet;
 * validate these values with PC Dump before selecting a final default.
 */
static const OV5640_Tuning_BrightnessCfg s_brightness_cfg[] =
{
    {0x10U, 0x09U},  /* -1 */
    {0x00U, 0x01U},  /*  0 */
    {0x10U, 0x01U}   /* +1 */
};

static const OV5640_Tuning_ContrastCfg s_contrast_cfg[] =
{
    {0x18U, 0x18U},  /* -1 */
    {0x20U, 0x20U},  /*  0 */
    {0x28U, 0x28U}   /* +1 */
};

static const OV5640_Tuning_SaturationCfg s_saturation_cfg[] =
{
    {0x30U, 0x30U},  /* 0 */
    {0x40U, 0x40U},  /* 1 */
    {0x60U, 0x60U}   /* 2 */
};

static const OV5640_Tuning_SharpnessCfg s_sharpness_cfg[] =
{
    {0x08U, 0x30U, 0x10U, 0x00U, 0x08U, 0x30U, 0x04U, 0x06U},  /* 0 */
    {0x08U, 0x20U, 0x18U, 0x04U, 0x08U, 0x20U, 0x08U, 0x08U},  /* 1 */
    {0x06U, 0x18U, 0x20U, 0x08U, 0x06U, 0x18U, 0x0CU, 0x0AU}   /* 2 */
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

static uint8_t OV5640_Tuning_WriteAwbGain(uint16_t r_gain,
                                          uint16_t g_gain,
                                          uint16_t b_gain)
{
    if (SCCB_WriteReg(0x3400U, (uint8_t)((r_gain >> 8) & 0x0FU)) != 0U) return 1U;
    if (SCCB_WriteReg(0x3401U, (uint8_t)(r_gain & 0xFFU)) != 0U) return 2U;
    if (SCCB_WriteReg(0x3402U, (uint8_t)((g_gain >> 8) & 0x0FU)) != 0U) return 3U;
    if (SCCB_WriteReg(0x3403U, (uint8_t)(g_gain & 0xFFU)) != 0U) return 4U;
    if (SCCB_WriteReg(0x3404U, (uint8_t)((b_gain >> 8) & 0x0FU)) != 0U) return 5U;
    if (SCCB_WriteReg(0x3405U, (uint8_t)(b_gain & 0xFFU)) != 0U) return 6U;
    return 0U;
}

uint8_t OV5640_Tuning_SetAWBMode(OV5640_AwbMode_t mode)
{
    const OV5640_Tuning_AwbGainCfg *cfg;
    uint8_t ret;

    if (mode == OV5640_AWB_MODE_AUTO)
    {
        return SCCB_WriteReg(0x3406U, 0x00U);
    }

    if ((mode < OV5640_AWB_MODE_SUNNY) || (mode > OV5640_AWB_MODE_HOME))
    {
        return 1U;
    }

    cfg = &s_awb_manual_gain_cfg[(uint32_t)mode - 1U];
    ret = OV5640_Tuning_WriteAwbGain(cfg->r_gain, cfg->g_gain, cfg->b_gain);
    if (ret != 0U)
    {
        return (uint8_t)(ret + 1U);
    }

    if (SCCB_WriteReg(0x3406U, 0x01U) != 0U)
    {
        return 8U;
    }

    return 0U;
}

void OV5640_Tuning_DumpAWBRegs(void)
{
    static const uint16_t regs[] =
    {
        0x3400U, 0x3401U, 0x3402U, 0x3403U, 0x3404U, 0x3405U, 0x3406U
    };
    uint8_t val;

    LOG_RAW("========== OV5640 AWB DUMP ==========\r\n");
    for (uint32_t i = 0U; i < (sizeof(regs) / sizeof(regs[0])); ++i)
    {
        if (SCCB_ReadReg(regs[i], &val) != 0U)
        {
            LOG_RAW("AWB reg 0x%04X read failed\r\n", (unsigned int)regs[i]);
            continue;
        }
        LOG_RAW("AWB reg 0x%04X = 0x%02X\r\n",
                (unsigned int)regs[i],
                (unsigned int)val);
    }
    LOG_RAW("=====================================\r\n");
}

static uint8_t OV5640_Tuning_LevelToIndex(int8_t level, uint32_t *index)
{
    if (index == 0)
    {
        return 1U;
    }

    if (level == (int8_t)OV5640_IMAGE_PARAM_MINUS_1)
    {
        *index = 0U;
        return 0U;
    }
    if (level == (int8_t)OV5640_IMAGE_PARAM_DEFAULT)
    {
        *index = 1U;
        return 0U;
    }
    if (level == (int8_t)OV5640_IMAGE_PARAM_PLUS_1)
    {
        *index = 2U;
        return 0U;
    }

    return 2U;
}

uint8_t OV5640_Tuning_SetBrightness(int8_t level)
{
    uint32_t index;
    const OV5640_Tuning_BrightnessCfg *cfg;

    if (OV5640_Tuning_LevelToIndex(level, &index) != 0U)
    {
        return 1U;
    }

    cfg = &s_brightness_cfg[index];
    if (SCCB_WriteReg(0x5580U, 0x06U) != 0U) return 2U;
    if (SCCB_WriteReg(0x5587U, cfg->offset) != 0U) return 3U;
    if (SCCB_WriteReg(0x5588U, cfg->sign) != 0U) return 4U;

    return 0U;
}

uint8_t OV5640_Tuning_SetContrast(int8_t level)
{
    uint32_t index;
    const OV5640_Tuning_ContrastCfg *cfg;

    if (OV5640_Tuning_LevelToIndex(level, &index) != 0U)
    {
        return 1U;
    }

    cfg = &s_contrast_cfg[index];
    if (SCCB_WriteReg(0x5580U, 0x06U) != 0U) return 2U;
    if (SCCB_WriteReg(0x5585U, cfg->center) != 0U) return 3U;
    if (SCCB_WriteReg(0x5586U, cfg->gain) != 0U) return 4U;
    if (SCCB_WriteReg(0x501DU, 0x40U) != 0U) return 5U;

    return 0U;
}

uint8_t OV5640_Tuning_SetSaturation(int8_t level)
{
    const OV5640_Tuning_SaturationCfg *cfg;

    if ((level < 0) || (level > 2))
    {
        return 1U;
    }

    cfg = &s_saturation_cfg[(uint32_t)level];
    if (SCCB_WriteReg(0x5580U, 0x06U) != 0U) return 2U;
    if (SCCB_WriteReg(0x5583U, cfg->u_gain) != 0U) return 3U;
    if (SCCB_WriteReg(0x5584U, cfg->v_gain) != 0U) return 4U;

    return 0U;
}

uint8_t OV5640_Tuning_SetSharpness(uint8_t level)
{
    const OV5640_Tuning_SharpnessCfg *cfg;

    if (level > 2U)
    {
        return 1U;
    }

    cfg = &s_sharpness_cfg[level];
    if (SCCB_WriteReg(0x5300U, cfg->mt_th1) != 0U) return 2U;
    if (SCCB_WriteReg(0x5301U, cfg->mt_th2) != 0U) return 3U;
    if (SCCB_WriteReg(0x5302U, cfg->mt_offset1) != 0U) return 4U;
    if (SCCB_WriteReg(0x5303U, cfg->mt_offset2) != 0U) return 5U;
    if (SCCB_WriteReg(0x5309U, cfg->th_th1) != 0U) return 6U;
    if (SCCB_WriteReg(0x530AU, cfg->th_th2) != 0U) return 7U;
    if (SCCB_WriteReg(0x530BU, cfg->th_offset1) != 0U) return 8U;
    if (SCCB_WriteReg(0x530CU, cfg->th_offset2) != 0U) return 9U;

    return 0U;
}
