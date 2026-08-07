#include "camera_sd_storage.h"

#include "bsp_sccb.h"
#include "camera_snapshot_control.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <string.h>

#define CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG 0x3018U
#define CAMERA_SD_DVP_CONFLICT_PAD_KEEP_MASK  0x8FU
#define CAMERA_SD_REG_VALUE_UNKNOWN           0xFFFFFFFFU

#define CAMERA_SD_CONFLICT_PIN_MASK \
    (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11)
#define CAMERA_SD_CONFLICT_MODE_MASK \
    ((3UL << (8U * 2U)) | (3UL << (9U * 2U)) | (3UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_AF_MODE \
    ((2UL << (8U * 2U)) | (2UL << (9U * 2U)) | (2UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_SPEED_VERY_HIGH CAMERA_SD_CONFLICT_MODE_MASK
#define CAMERA_SD_CONFLICT_PULLUP \
    ((1UL << (8U * 2U)) | (1UL << (9U * 2U)) | (1UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_AFRH_MASK \
    ((0xFUL << 0U) | (0xFUL << 4U) | (0xFUL << 12U))
#define CAMERA_SD_CONFLICT_AFRH_AF12 \
    ((12UL << 0U) | (12UL << 4U) | (12UL << 12U))
#define CAMERA_SD_CONFLICT_AFRH_AF13 \
    ((13UL << 0U) | (13UL << 4U) | (13UL << 12U))

#define CAMERA_SD_FULL_GPIOC_PIN_MASK \
    (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12)
#define CAMERA_SD_FULL_GPIOC_MODE_MASK \
    ((3UL << (8U * 2U)) | (3UL << (9U * 2U)) | \
     (3UL << (10U * 2U)) | (3UL << (11U * 2U)) | \
     (3UL << (12U * 2U)))
#define CAMERA_SD_FULL_GPIOD_PIN_MASK  GPIO_PIN_2
#define CAMERA_SD_FULL_GPIOD_MODE_MASK (3UL << (2U * 2U))

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t pin_number;
} CameraSdLine_t;

static const CameraSdLine_t s_camera_sd_lines[] =
{
    {GPIOC, 8U},
    {GPIOC, 9U},
    {GPIOC, 10U},
    {GPIOC, 11U},
    {GPIOC, 12U},
    {GPIOD, 2U}
};

static CameraSdStorageStatus_t s_camera_sd_status;
static SD_HandleTypeDef s_camera_sd_handle;
static uint32_t s_camera_sd_full_gpio_af12_selected;
static uint32_t s_camera_sd_clock_enabled;

static void Camera_SDStorage_SetLastError(uint32_t error_code)
{
    s_camera_sd_status.last_error_code = error_code;
    s_camera_sd_status.last_error_text =
        Camera_SDStorage_ErrorToString(error_code);
}

static uint32_t Camera_SDStorage_GetPinMode(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->MODER >> (pin_number * 2U)) & 0x3U;
}

static uint32_t Camera_SDStorage_GetPinPull(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->PUPDR >> (pin_number * 2U)) & 0x3U;
}

static uint32_t Camera_SDStorage_GetPinSpeed(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->OSPEEDR >> (pin_number * 2U)) & 0x3U;
}

static uint32_t Camera_SDStorage_GetPinAf(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    if (pin_number < 8U)
    {
        return (port->AFR[0] >> (pin_number * 4U)) & 0xFU;
    }

    return (port->AFR[1] >> ((pin_number - 8U) * 4U)) & 0xFU;
}

static uint32_t Camera_SDStorage_VerifyFullGpioAf12(void)
{
    uint32_t index;

    for (index = 0U;
         index < (sizeof(s_camera_sd_lines) / sizeof(s_camera_sd_lines[0]));
         ++index)
    {
        const CameraSdLine_t *line = &s_camera_sd_lines[index];

        if ((Camera_SDStorage_GetPinMode(line->port, line->pin_number) != 2U) ||
            (Camera_SDStorage_GetPinPull(line->port, line->pin_number) !=
             GPIO_PULLUP) ||
            (Camera_SDStorage_GetPinSpeed(line->port, line->pin_number) !=
             GPIO_SPEED_FREQ_VERY_HIGH) ||
            (Camera_SDStorage_GetPinAf(line->port, line->pin_number) != 12U) ||
            (((line->port->OTYPER >> line->pin_number) & 0x1U) != 0U))
        {
            return 0U;
        }
    }

    return 1U;
}

static uint32_t Camera_SDStorage_ReleaseConflictPins(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) != 0U) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) != 0U))
    {
        return CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED;
    }

    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_SwitchConflictPinsToSdioAf12(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_AF_MODE) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_PULLUP) ||
        ((GPIOC->OSPEEDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_SPEED_VERY_HIGH) ||
        ((GPIOC->OTYPER & CAMERA_SD_CONFLICT_PIN_MASK) != 0U) ||
        ((GPIOC->AFR[1] & CAMERA_SD_CONFLICT_AFRH_MASK) !=
         CAMERA_SD_CONFLICT_AFRH_AF12))
    {
        return CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED;
    }

    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_SwitchFullSdioGpioToAf12(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    gpio.Pin = CAMERA_SD_FULL_GPIOC_PIN_MASK;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = CAMERA_SD_FULL_GPIOD_PIN_MASK;
    HAL_GPIO_Init(GPIOD, &gpio);

    if (Camera_SDStorage_VerifyFullGpioAf12() == 0U)
    {
        s_camera_sd_full_gpio_af12_selected = 0U;
        return CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
    }

    s_camera_sd_full_gpio_af12_selected = 1U;
    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_LeaveFullSdioGpioToInput(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = CAMERA_SD_FULL_GPIOC_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = CAMERA_SD_FULL_GPIOD_PIN_MASK;
    HAL_GPIO_Init(GPIOD, &gpio);

    s_camera_sd_full_gpio_af12_selected = 0U;
    if (((GPIOC->MODER & CAMERA_SD_FULL_GPIOC_MODE_MASK) != 0U) ||
        ((GPIOC->PUPDR & CAMERA_SD_FULL_GPIOC_MODE_MASK) != 0U) ||
        ((GPIOD->MODER & CAMERA_SD_FULL_GPIOD_MODE_MASK) != 0U) ||
        ((GPIOD->PUPDR & CAMERA_SD_FULL_GPIOD_MODE_MASK) != 0U))
    {
        return CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED;
    }

    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_RestoreConflictPins(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF13_DCMI;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_AF_MODE) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) != 0U) ||
        ((GPIOC->OSPEEDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_SPEED_VERY_HIGH) ||
        ((GPIOC->OTYPER & CAMERA_SD_CONFLICT_PIN_MASK) != 0U) ||
        ((GPIOC->AFR[1] & CAMERA_SD_CONFLICT_AFRH_MASK) !=
         CAMERA_SD_CONFLICT_AFRH_AF13))
    {
        return CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED;
    }

    return CAMERA_SD_OK;
}

static void Camera_SDStorage_PrepareSdHandle(void)
{
    s_camera_sd_handle.Instance = SDIO;
    s_camera_sd_handle.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    s_camera_sd_handle.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    s_camera_sd_handle.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    s_camera_sd_handle.Init.BusWide = SDIO_BUS_WIDE_1B;
    s_camera_sd_handle.Init.HardwareFlowControl =
        SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    s_camera_sd_handle.Init.ClockDiv = CAMERA_SD_INIT_CLOCK_DIV;
}

static void Camera_SDStorage_EnableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_ENABLE();
    s_camera_sd_clock_enabled = 1U;
}

static void Camera_SDStorage_DisableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_DISABLE();
    s_camera_sd_clock_enabled = 0U;
}

void Camera_SDStorage_InitState(void)
{
    memset(&s_camera_sd_status, 0, sizeof(s_camera_sd_status));
    memset(&s_camera_sd_handle, 0, sizeof(s_camera_sd_handle));

    s_camera_sd_status.supported = 1U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.dvp_mask_supported = 1U;
    s_camera_sd_status.dvp_mask_reg_3018_saved =
        CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.dvp_mask_restored = 1U;
    s_camera_sd_status.last_init_result = CAMERA_SD_ERR_NOT_IMPLEMENTED;
    s_camera_sd_status.last_io_result = CAMERA_SD_ERR_NOT_IMPLEMENTED;
    s_camera_sd_full_gpio_af12_selected = 0U;
    s_camera_sd_clock_enabled = 0U;
    Camera_SDStorage_SetLastError(CAMERA_SD_OK);
}

void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status)
{
    if (status != NULL)
    {
        *status = s_camera_sd_status;
    }
}

uint32_t Camera_SDStorage_StopDvpConflictPads(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint8_t register_value = 0U;
    uint8_t masked_value;

    if (Camera_SnapshotControl_IsTakeoverPreconditionReady() == 0U)
    {
        result = CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
    }
    else if (SCCB_ReadReg(
                 CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG,
                 &register_value) != 0U)
    {
        result = CAMERA_SD_ERR_SENSOR_REG_READ_FAILED;
    }
    else
    {
        if (s_camera_sd_status.dvp_mask_active == 0U)
        {
            s_camera_sd_status.dvp_mask_reg_3018_saved = register_value;
        }

        masked_value = (uint8_t)(
            register_value & CAMERA_SD_DVP_CONFLICT_PAD_KEEP_MASK);
        if (SCCB_WriteReg(
                CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG,
                masked_value) != 0U)
        {
            result = CAMERA_SD_ERR_SENSOR_REG_WRITE_FAILED;
        }
        else
        {
            HAL_Delay(5U);
            if (SCCB_ReadReg(
                    CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG,
                    &register_value) != 0U)
            {
                result = CAMERA_SD_ERR_SENSOR_REG_READ_FAILED;
            }
            else if (register_value != masked_value)
            {
                result = CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED;
            }
        }
    }

    if (result == CAMERA_SD_OK)
    {
        s_camera_sd_status.dvp_mask_active = 1U;
        s_camera_sd_status.dvp_mask_restored = 0U;
    }
    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RestoreDvpConflictPads(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint8_t register_value = 0U;
    uint8_t saved_value;

    if (s_camera_sd_status.dvp_mask_reg_3018_saved ==
        CAMERA_SD_REG_VALUE_UNKNOWN)
    {
        result = CAMERA_SD_ERR_NO_SAVED_3018;
    }
    else if (s_camera_sd_full_gpio_af12_selected != 0U)
    {
        result = CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE;
    }
    else
    {
        saved_value =
            (uint8_t)s_camera_sd_status.dvp_mask_reg_3018_saved;
        if (SCCB_WriteReg(
                CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG,
                saved_value) != 0U)
        {
            result = CAMERA_SD_ERR_SENSOR_REG_WRITE_FAILED;
        }
        else
        {
            HAL_Delay(5U);
            if (SCCB_ReadReg(
                    CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG,
                    &register_value) != 0U)
            {
                result = CAMERA_SD_ERR_SENSOR_REG_READ_FAILED;
            }
            else if (register_value != saved_value)
            {
                result = CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED;
            }
        }
    }

    if (result == CAMERA_SD_OK)
    {
        s_camera_sd_status.dvp_mask_active = 0U;
        s_camera_sd_status.dvp_mask_restored = 1U;
    }
    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RequestTakeoverEnter(void)
{
    uint32_t result;

    if (Camera_SnapshotControl_IsTakeoverPreconditionReady() == 0U)
    {
        result = CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
    }
    else
    {
        result = Camera_SDStorage_ReleaseConflictPins();
        if (result == CAMERA_SD_OK)
        {
            result = Camera_SDStorage_SwitchConflictPinsToSdioAf12();
        }
        if (result == CAMERA_SD_OK)
        {
            result = Camera_SDStorage_SwitchFullSdioGpioToAf12();
        }
    }

    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RequestInit(void)
{
    HAL_StatusTypeDef hal_status;
    HAL_SD_CardInfoTypeDef card_info;
    uint32_t result;

    if (s_camera_sd_full_gpio_af12_selected == 0U)
    {
        result = CAMERA_SD_ERR_NEED_TAKEOVER;
    }
    else
    {
        Camera_SDStorage_PrepareSdHandle();
        Camera_SDStorage_EnableSdioClock();
        hal_status = HAL_SD_Init(&s_camera_sd_handle);
        if (hal_status != HAL_OK)
        {
            s_camera_sd_status.card_ready = 0U;
            s_camera_sd_status.sdio_ready = 0U;
            result = CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED;
        }
        else
        {
            s_camera_sd_status.sdio_ready = 1U;
            hal_status = HAL_SD_GetCardInfo(&s_camera_sd_handle, &card_info);
            if (hal_status == HAL_OK)
            {
                s_camera_sd_status.card_ready = 1U;
                result = CAMERA_SD_OK;
            }
            else
            {
                s_camera_sd_status.card_ready = 0U;
                result = CAMERA_SD_ERR_CARD_INFO_FAILED;
            }
        }
    }

    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_init_result = result;
    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RequestTakeoverExit(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint32_t gpio_result;

    if (s_camera_sd_status.sdio_ready != 0U)
    {
        if (HAL_SD_DeInit(&s_camera_sd_handle) != HAL_OK)
        {
            result = CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED;
        }
    }

    if (s_camera_sd_clock_enabled != 0U)
    {
        Camera_SDStorage_DisableSdioClock();
    }
    s_camera_sd_status.card_ready = 0U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;

    if (s_camera_sd_full_gpio_af12_selected != 0U)
    {
        gpio_result = Camera_SDStorage_LeaveFullSdioGpioToInput();
        if (gpio_result != CAMERA_SD_OK)
        {
            result = gpio_result;
        }
    }

    gpio_result = Camera_SDStorage_RestoreConflictPins();
    if (gpio_result != CAMERA_SD_OK)
    {
        result = gpio_result;
    }

    Camera_SDStorage_SetLastError(result);
    return result;
}

const char *Camera_SDStorage_ErrorToString(uint32_t error_code)
{
    switch (error_code)
    {
        case CAMERA_SD_OK:
            return "OK";
        case CAMERA_SD_ERR_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case CAMERA_SD_ERR_NEED_TAKEOVER:
            return "NEED_TAKEOVER";
        case CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE:
            return "TAKEOVER_ALREADY_ACTIVE";
        case CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED:
            return "SNAPSHOT_NOT_PAUSED";
        case CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED:
            return "CONFLICT_PIN_RELEASE_FAILED";
        case CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED:
            return "CONFLICT_PIN_RESTORE_FAILED";
        case CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED:
            return "SDIO_AF12_SWITCH_FAILED";
        case CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED:
            return "SDIO_AF12_RESTORE_FAILED";
        case CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED:
            return "SDIO_FULL_GPIO_SWITCH_FAILED";
        case CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED:
            return "SDIO_FULL_GPIO_RESTORE_FAILED";
        case CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED:
            return "SDIO_HAL_INIT_FAILED";
        case CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED:
            return "SDIO_HAL_DEINIT_FAILED";
        case CAMERA_SD_ERR_CARD_INFO_FAILED:
            return "CARD_INFO_FAILED";
        case CAMERA_SD_ERR_SENSOR_REG_READ_FAILED:
            return "SENSOR_REG_READ_FAILED";
        case CAMERA_SD_ERR_SENSOR_REG_WRITE_FAILED:
            return "SENSOR_REG_WRITE_FAILED";
        case CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED:
            return "SENSOR_REG_VERIFY_FAILED";
        case CAMERA_SD_ERR_NO_SAVED_3018:
            return "NO_SAVED_3018";
        default:
            return "UNKNOWN_ERROR";
    }
}
