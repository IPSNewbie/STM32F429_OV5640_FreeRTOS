#include "camera_sd_storage.h"

#include "bsp_sccb.h"
#include "camera_dcmi_dma.h"
#include "camera_frame_buffer.h"
#include "camera_rtos.h"
#include "camera_snapshot_control.h"
#include "ff.h"
#include "diskio.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <string.h>

#define CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG 0x3018U
#define CAMERA_SD_DVP_CONFLICT_PAD_KEEP_MASK  0x8FU
#define CAMERA_SD_REG_VALUE_UNKNOWN           0xFFFFFFFFU
#define CAMERA_SD_FATFS_CARD_TIMEOUT_MS       1000U
#define CAMERA_SD_FATFS_READ_TIMEOUT_MS       1000U
#define CAMERA_SD_FATFS_WRITE_TIMEOUT_MS      1000U
#define CAMERA_SD_FATFS_MOUNT_NOT_RUN         0xFFFFFFFFU
#define CAMERA_SD_RESTORE_LCD_WIDTH            480U
#define CAMERA_SD_RESTORE_LCD_HEIGHT           320U
#define CAMERA_SD_CAMERA_RESTORE_DELAY_MS       100U
#define CAMERA_SD_SNAPSHOT_FILE_NAME           "IMAGE.RGB"
#define CAMERA_SD_SNAPSHOT_FORMAT_TEXT         "RGB565"
#define CAMERA_SD_SNAPSHOT_SOURCE_TEXT         "FRONT"
#define CAMERA_SD_FRAME_PREPARE_MAX_RETRIES      3U
#define CAMERA_SD_FRAME_PREPARE_RETRY_DELAY_MS  75U

#define CAMERA_SD_CONFLICT_PIN_MASK \
    (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11)
#define CAMERA_SD_CONFLICT_MODE_MASK \
    ((3UL << (8U * 2U)) | (3UL << (9U * 2U)) | (3UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_AF_MODE \
    ((2UL << (8U * 2U)) | (2UL << (9U * 2U)) | (2UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_SPEED_HIGH \
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

static uint8_t s_camera_sd_snapshot_image_buffer[CAMERA_FB_SIZE_BYTES]
    __attribute__((aligned(4)));
static CameraSdStorageStatus_t s_camera_sd_status;
static SD_HandleTypeDef s_camera_sd_handle;
static HAL_SD_CardInfoTypeDef s_camera_sd_card_info;
static FATFS s_camera_sd_fatfs;
static FIL s_camera_sd_file;
static uint32_t s_camera_sd_full_gpio_af12_selected;
static uint32_t s_camera_sd_clock_enabled;
static uint32_t s_camera_sd_hal_init_attempted;
static uint32_t s_camera_sd_card_info_valid;
static uint32_t s_camera_sd_fatfs_session_active;
static uint32_t s_camera_sd_fatfs_write_allowed;
static uint32_t s_camera_sd_fatfs_disk_error;

static void Camera_SDStorage_SetLastError(uint32_t error_code)
{
    s_camera_sd_status.last_error_code = error_code;
    s_camera_sd_status.last_error_text =
        Camera_SDStorage_ErrorToString(error_code);
}

static void Camera_SDStorage_SetSaveError(uint32_t error_code)
{
    s_camera_sd_status.save_error_code = error_code;
    s_camera_sd_status.save_error_text =
        Camera_SDStorage_ErrorToString(error_code);
}

static void Camera_SDStorage_SetMountResult(FRESULT mount_result)
{
    s_camera_sd_status.last_mount_result = (uint32_t)mount_result;
    s_camera_sd_status.last_mount_text =
        (mount_result == FR_OK) ? "PASS" : "FAIL";
}

static void Camera_SDStorage_RecordFirstError(
    uint32_t *first_error,
    uint32_t candidate)
{
    if ((first_error != NULL) &&
        (*first_error == CAMERA_SD_OK) &&
        (candidate != CAMERA_SD_OK))
    {
        *first_error = candidate;
    }
}

static void Camera_SDStorage_EnsureDcmiDmaHandle(void)
{
    if (g_camera_dcmi.DMA_Handle == NULL)
    {
        __HAL_LINKDMA(&g_camera_dcmi, DMA_Handle, g_camera_dma);
    }
}

static uint32_t Camera_SDStorage_WaitForCardTransfer(void)
{
    uint32_t start_tick = HAL_GetTick();
    HAL_SD_CardStateTypeDef card_state;

    for (;;)
    {
        card_state = HAL_SD_GetCardState(&s_camera_sd_handle);
        if (card_state == HAL_SD_CARD_TRANSFER)
        {
            return CAMERA_SD_OK;
        }
        if ((card_state == HAL_SD_CARD_ERROR) ||
            (card_state == HAL_SD_CARD_DISCONNECTED))
        {
            return CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
        }
        if ((HAL_GetTick() - start_tick) >=
            CAMERA_SD_FATFS_CARD_TIMEOUT_MS)
        {
            return CAMERA_SD_ERR_FATFS_CARD_TIMEOUT;
        }

        HAL_Delay(1U);
    }
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
             GPIO_SPEED_FREQ_HIGH) ||
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
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_AF_MODE) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_PULLUP) ||
        ((GPIOC->OSPEEDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_SPEED_HIGH) ||
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
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = CAMERA_SD_FULL_GPIOD_PIN_MASK;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* Mark takeover before verification so cleanup restores partial switches. */
    s_camera_sd_full_gpio_af12_selected = 1U;
    if (Camera_SDStorage_VerifyFullGpioAf12() == 0U)
    {
        return CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
    }

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

static uint32_t Camera_SDStorage_RestoreCameraLink(
    uint32_t restore_continuous_capture,
    uint32_t verify_sdio_inputs)
{
    uint32_t result = CAMERA_SD_OK;

    (void)Camera_SnapshotControl_RequestRestore();
    Camera_SDStorage_EnsureDcmiDmaHandle();
    Camera_DCMI_Init();
    if (restore_continuous_capture != 0U)
    {
        Camera_DCMI_StartToLCD(
            0U,
            0U,
            CAMERA_SD_RESTORE_LCD_WIDTH,
            CAMERA_SD_RESTORE_LCD_HEIGHT);
    }

    HAL_Delay(CAMERA_SD_CAMERA_RESTORE_DELAY_MS);

    if ((Camera_SnapshotControl_IsSoftwareGuardActive() != 0U) ||
        (s_camera_sd_status.dvp_mask_active != 0U) ||
        (g_camera_dcmi.Instance != DCMI) ||
        (g_camera_dcmi.DMA_Handle != &g_camera_dma) ||
        (g_camera_dcmi.State != HAL_DCMI_STATE_READY) ||
        ((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_AF_MODE) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_PULLUP) ||
        ((GPIOC->OSPEEDR & CAMERA_SD_CONFLICT_MODE_MASK) !=
         CAMERA_SD_CONFLICT_SPEED_VERY_HIGH) ||
        ((GPIOC->OTYPER & CAMERA_SD_CONFLICT_PIN_MASK) != 0U) ||
        ((GPIOC->AFR[1] & CAMERA_SD_CONFLICT_AFRH_MASK) !=
         CAMERA_SD_CONFLICT_AFRH_AF13))
    {
        result = CAMERA_SD_ERR_CAMERA_RESTORE_FAILED;
    }
    else if ((restore_continuous_capture != 0U) &&
             (((DCMI->CR & DCMI_CR_CAPTURE) == 0U) ||
              ((DMA2_Stream1->CR & DMA_SxCR_EN) == 0U)))
    {
        result = CAMERA_SD_ERR_CAMERA_RESTORE_FAILED;
    }
    else if ((restore_continuous_capture == 0U) &&
             ((DCMI->CR & DCMI_CR_CAPTURE) != 0U))
    {
        result = CAMERA_SD_ERR_CAMERA_RESTORE_FAILED;
    }
    else if ((verify_sdio_inputs != 0U) &&
             ((Camera_SDStorage_GetPinMode(GPIOC, 10U) != 0U) ||
              (Camera_SDStorage_GetPinPull(GPIOC, 10U) != GPIO_NOPULL) ||
              (Camera_SDStorage_GetPinMode(GPIOC, 12U) != 0U) ||
              (Camera_SDStorage_GetPinPull(GPIOC, 12U) != GPIO_NOPULL) ||
              (Camera_SDStorage_GetPinMode(GPIOD, 2U) != 0U) ||
              (Camera_SDStorage_GetPinPull(GPIOD, 2U) != GPIO_NOPULL)))
    {
        result = CAMERA_SD_ERR_CAMERA_RESTORE_FAILED;
    }

    return result;
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
    memset(&s_camera_sd_card_info, 0, sizeof(s_camera_sd_card_info));
    memset(&s_camera_sd_fatfs, 0, sizeof(s_camera_sd_fatfs));
    memset(&s_camera_sd_file, 0, sizeof(s_camera_sd_file));

    s_camera_sd_status.supported = 1U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.dvp_mask_available = 1U;
    s_camera_sd_status.dvp_reg_3018_saved =
        CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.dvp_reg_3018_current_or_restored =
        CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.last_mount_result =
        CAMERA_SD_FATFS_MOUNT_NOT_RUN;
    s_camera_sd_status.last_mount_text = "NOT_RUN";
    s_camera_sd_status.last_snapshot_text = "NOT_RUN";
    s_camera_sd_status.last_file_name = "NONE";
    s_camera_sd_status.last_file_size = 0U;
    s_camera_sd_status.save_count = 0U;
    s_camera_sd_status.last_sd_init_status = CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.last_sd_init_error = CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.last_sd_rw_status = CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_status.last_sd_rw_error = CAMERA_SD_REG_VALUE_UNKNOWN;
    s_camera_sd_full_gpio_af12_selected = 0U;
    s_camera_sd_clock_enabled = 0U;
    s_camera_sd_hal_init_attempted = 0U;
    s_camera_sd_card_info_valid = 0U;
    s_camera_sd_fatfs_session_active = 0U;
    s_camera_sd_fatfs_write_allowed = 0U;
    s_camera_sd_fatfs_disk_error = CAMERA_SD_OK;
    Camera_SDStorage_SetSaveError(CAMERA_SD_OK);
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
            s_camera_sd_status.dvp_reg_3018_saved = register_value;
        }
        s_camera_sd_status.dvp_reg_3018_current_or_restored = register_value;

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
            else
            {
                s_camera_sd_status.dvp_reg_3018_current_or_restored =
                    register_value;
                if (register_value != masked_value)
                {
                    result = CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED;
                }
            }
        }
    }

    if (result == CAMERA_SD_OK)
    {
        s_camera_sd_status.dvp_mask_active = 1U;
    }
    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RestoreDvpConflictPads(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint8_t register_value = 0U;
    uint8_t saved_value;

    if (s_camera_sd_status.dvp_reg_3018_saved ==
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
            (uint8_t)s_camera_sd_status.dvp_reg_3018_saved;
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
            else
            {
                s_camera_sd_status.dvp_reg_3018_current_or_restored =
                    register_value;
                if (register_value != saved_value)
                {
                    result = CAMERA_SD_ERR_SENSOR_REG_VERIFY_FAILED;
                }
            }
        }
    }

    if (result == CAMERA_SD_OK)
    {
        s_camera_sd_status.dvp_mask_active = 0U;
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
    uint32_t result;

    s_camera_sd_card_info_valid = 0U;

    if (s_camera_sd_full_gpio_af12_selected == 0U)
    {
        result = CAMERA_SD_ERR_NEED_TAKEOVER;
    }
    else
    {
        Camera_SDStorage_PrepareSdHandle();
        Camera_SDStorage_EnableSdioClock();
        s_camera_sd_hal_init_attempted = 1U;
        hal_status = HAL_SD_Init(&s_camera_sd_handle);
        s_camera_sd_status.last_sd_init_status = (uint32_t)hal_status;
        s_camera_sd_status.last_sd_init_error =
            HAL_SD_GetError(&s_camera_sd_handle);
        if (hal_status != HAL_OK)
        {
            s_camera_sd_status.card_ready = 0U;
            s_camera_sd_status.sdio_ready = 0U;
            result = CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED;
        }
        else
        {
            s_camera_sd_status.sdio_ready = 1U;
            hal_status = HAL_SD_GetCardInfo(
                &s_camera_sd_handle,
                &s_camera_sd_card_info);
            if (hal_status == HAL_OK)
            {
                result = Camera_SDStorage_WaitForCardTransfer();
                if (result == CAMERA_SD_OK)
                {
                    s_camera_sd_status.card_ready = 1U;
                    s_camera_sd_card_info_valid = 1U;
                }
                else
                {
                    s_camera_sd_status.card_ready = 0U;
                }
            }
            else
            {
                s_camera_sd_status.card_ready = 0U;
                result = CAMERA_SD_ERR_CARD_INFO_FAILED;
            }
        }
    }

    s_camera_sd_status.fatfs_ready = 0U;
    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_RequestTakeoverExit(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint32_t gpio_result;

    if ((s_camera_sd_hal_init_attempted != 0U) ||
        (s_camera_sd_clock_enabled != 0U))
    {
        if (HAL_SD_DeInit(&s_camera_sd_handle) != HAL_OK)
        {
            Camera_SDStorage_RecordFirstError(
                &result,
                CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED);
        }
        s_camera_sd_hal_init_attempted = 0U;
    }

    Camera_SDStorage_DisableSdioClock();
    if ((RCC->APB2ENR & RCC_APB2ENR_SDIOEN) != 0U)
    {
        Camera_SDStorage_RecordFirstError(
            &result,
            CAMERA_SD_ERR_SDIO_CLOCK_DISABLE_FAILED);
    }
    s_camera_sd_status.card_ready = 0U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_card_info_valid = 0U;
    s_camera_sd_fatfs_session_active = 0U;
    s_camera_sd_fatfs_write_allowed = 0U;

    gpio_result = Camera_SDStorage_LeaveFullSdioGpioToInput();
    Camera_SDStorage_RecordFirstError(&result, gpio_result);

    gpio_result = Camera_SDStorage_RestoreConflictPins();
    Camera_SDStorage_RecordFirstError(&result, gpio_result);

    Camera_SDStorage_SetLastError(result);
    return result;
}

uint32_t Camera_SDStorage_FatFsDiskStatus(void)
{
    if ((s_camera_sd_fatfs_session_active == 0U) ||
        (s_camera_sd_status.sdio_ready == 0U) ||
        (s_camera_sd_card_info_valid == 0U))
    {
        return CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
    }

    return (HAL_SD_GetCardState(&s_camera_sd_handle) ==
            HAL_SD_CARD_TRANSFER) ?
        CAMERA_SD_OK : CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
}

uint32_t Camera_SDStorage_FatFsDiskInitialize(void)
{
    uint32_t result;

    if ((s_camera_sd_fatfs_session_active == 0U) ||
        (s_camera_sd_status.sdio_ready == 0U) ||
        (s_camera_sd_card_info_valid == 0U))
    {
        result = CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
    }
    else
    {
        result = Camera_SDStorage_WaitForCardTransfer();
    }

    if (result != CAMERA_SD_OK)
    {
        s_camera_sd_fatfs_disk_error = result;
    }
    return result;
}

uint32_t Camera_SDStorage_FatFsDiskRead(
    uint8_t *buffer,
    uint32_t sector,
    uint32_t count)
{
    HAL_StatusTypeDef hal_status;
    uint32_t result;

    if ((buffer == NULL) || (count == 0U))
    {
        result = CAMERA_SD_ERR_INVALID_ARGUMENT;
    }
    else if ((s_camera_sd_fatfs_session_active == 0U) ||
             (s_camera_sd_status.sdio_ready == 0U) ||
             (s_camera_sd_card_info_valid == 0U))
    {
        result = CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
    }
    else
    {
        result = Camera_SDStorage_WaitForCardTransfer();
        if (result == CAMERA_SD_OK)
        {
            hal_status = HAL_SD_ReadBlocks(
                &s_camera_sd_handle,
                buffer,
                sector,
                count,
                CAMERA_SD_FATFS_READ_TIMEOUT_MS);
            s_camera_sd_status.last_sd_rw_status = (uint32_t)hal_status;
            s_camera_sd_status.last_sd_rw_error =
                HAL_SD_GetError(&s_camera_sd_handle);
            if (hal_status != HAL_OK)
            {
                result = CAMERA_SD_ERR_FATFS_DISK_READ_FAILED;
            }
            else
            {
                result = Camera_SDStorage_WaitForCardTransfer();
            }
        }
    }

    if (result != CAMERA_SD_OK)
    {
        s_camera_sd_fatfs_disk_error = result;
    }
    return result;
}

uint32_t Camera_SDStorage_FatFsDiskWrite(
    const uint8_t *buffer,
    uint32_t sector,
    uint32_t count)
{
    HAL_StatusTypeDef hal_status;
    uint32_t result;

    if ((buffer == NULL) || (count == 0U))
    {
        result = CAMERA_SD_ERR_INVALID_ARGUMENT;
    }
    else if ((s_camera_sd_fatfs_session_active == 0U) ||
             (s_camera_sd_fatfs_write_allowed == 0U) ||
             (s_camera_sd_status.sdio_ready == 0U) ||
             (s_camera_sd_card_info_valid == 0U))
    {
        result = CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
    }
    else
    {
        result = Camera_SDStorage_WaitForCardTransfer();
        if (result == CAMERA_SD_OK)
        {
            hal_status = HAL_SD_WriteBlocks(
                &s_camera_sd_handle,
                (uint8_t *)buffer,
                sector,
                count,
                CAMERA_SD_FATFS_WRITE_TIMEOUT_MS);
            s_camera_sd_status.last_sd_rw_status = (uint32_t)hal_status;
            s_camera_sd_status.last_sd_rw_error =
                HAL_SD_GetError(&s_camera_sd_handle);
            if (hal_status != HAL_OK)
            {
                result = CAMERA_SD_ERR_FATFS_DISK_WRITE_FAILED;
            }
            else
            {
                result = Camera_SDStorage_WaitForCardTransfer();
            }
        }
    }

    if (result != CAMERA_SD_OK)
    {
        s_camera_sd_fatfs_disk_error = result;
        Camera_SDStorage_SetSaveError(result);
        Camera_SDStorage_SetLastError(result);
    }
    return result;
}

uint32_t Camera_SDStorage_FatFsDiskIoctl(uint8_t command, void *buffer)
{
    uint32_t result = CAMERA_SD_OK;

    if ((s_camera_sd_fatfs_session_active == 0U) ||
        (s_camera_sd_status.sdio_ready == 0U) ||
        (s_camera_sd_card_info_valid == 0U))
    {
        result = CAMERA_SD_ERR_FATFS_DISK_NOT_READY;
    }
    else if (command == CTRL_SYNC)
    {
        result = Camera_SDStorage_WaitForCardTransfer();
    }
    else if (buffer == NULL)
    {
        result = CAMERA_SD_ERR_INVALID_ARGUMENT;
    }
    else
    {
        switch (command)
        {
            case GET_SECTOR_COUNT:
                *((uint32_t *)buffer) = s_camera_sd_card_info.LogBlockNbr;
                break;

            case GET_SECTOR_SIZE:
                *((uint16_t *)buffer) =
                    (uint16_t)s_camera_sd_card_info.LogBlockSize;
                break;

            case GET_BLOCK_SIZE:
                *((uint32_t *)buffer) = 1U;
                break;

            default:
                result = CAMERA_SD_ERR_FATFS_DISK_IOCTL_FAILED;
                break;
        }
    }

    if (result != CAMERA_SD_OK)
    {
        s_camera_sd_fatfs_disk_error = result;
    }
    return result;
}

uint32_t Camera_SDStorage_CheckFatfsMount(void)
{
    uint32_t result = CAMERA_SD_OK;
    uint32_t step_result;
    uint32_t prepare_attempted = 0U;
    uint32_t dvp_restore_required = 0U;
    uint32_t takeover_attempted = 0U;
    uint32_t mount_attempted = 0U;
    uint32_t restore_continuous_capture;
    FRESULT mount_result;

    s_camera_sd_status.card_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_mount_result =
        CAMERA_SD_FATFS_MOUNT_NOT_RUN;
    s_camera_sd_status.last_mount_text = "NOT_RUN";
    s_camera_sd_fatfs_disk_error = CAMERA_SD_OK;

    if ((Camera_SnapshotControl_IsSoftwareGuardActive() != 0U) ||
        (s_camera_sd_fatfs_session_active != 0U) ||
        (s_camera_sd_full_gpio_af12_selected != 0U) ||
        (s_camera_sd_clock_enabled != 0U) ||
        (s_camera_sd_status.dvp_mask_active != 0U))
    {
        result = CAMERA_SD_ERR_SNAPSHOT_BUSY;
        Camera_SDStorage_SetLastError(result);
        return result;
    }

    s_camera_sd_status.dvp_reg_3018_saved =
        CAMERA_SD_REG_VALUE_UNKNOWN;
    restore_continuous_capture =
        ((DCMI->CR & DCMI_CR_CAPTURE) != 0U) ? 1U : 0U;
    Camera_SDStorage_EnsureDcmiDmaHandle();
    prepare_attempted = 1U;
    step_result = Camera_SnapshotControl_RequestPrepare();
    if (step_result != CAMERA_SNAPSHOT_OK)
    {
        result = CAMERA_SD_ERR_SNAPSHOT_PREPARE_FAILED;
        goto cleanup;
    }

    step_result = Camera_SDStorage_StopDvpConflictPads();
    if (s_camera_sd_status.dvp_reg_3018_saved !=
        CAMERA_SD_REG_VALUE_UNKNOWN)
    {
        dvp_restore_required = 1U;
    }
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    takeover_attempted = 1U;
    step_result = Camera_SDStorage_RequestTakeoverEnter();
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    step_result = Camera_SDStorage_RequestInit();
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    s_camera_sd_fatfs_session_active = 1U;
    mount_attempted = 1U;
    mount_result = f_mount(&s_camera_sd_fatfs, "", 1U);
    Camera_SDStorage_SetMountResult(mount_result);
    if (mount_result != FR_OK)
    {
        result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_MOUNT_FAILED;
        goto cleanup;
    }

    s_camera_sd_status.fatfs_ready = 1U;

cleanup:
    if (mount_attempted != 0U)
    {
        mount_result = f_mount(NULL, "", 0U);
        if (mount_result != FR_OK)
        {
            Camera_SDStorage_RecordFirstError(
                &result,
                CAMERA_SD_ERR_FATFS_UNMOUNT_FAILED);
        }
        s_camera_sd_status.fatfs_ready = 0U;
    }
    s_camera_sd_fatfs_session_active = 0U;

    if (takeover_attempted != 0U)
    {
        step_result = Camera_SDStorage_RequestTakeoverExit();
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    if (dvp_restore_required != 0U)
    {
        step_result = Camera_SDStorage_RestoreDvpConflictPads();
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    if (prepare_attempted != 0U)
    {
        step_result = Camera_SDStorage_RestoreCameraLink(
            restore_continuous_capture,
            takeover_attempted);
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    s_camera_sd_status.card_ready =
        (result == CAMERA_SD_OK) ? 1U : 0U;
    Camera_SDStorage_SetLastError(result);
    return result;
}

static uint32_t Camera_SDStorage_GetValidatedFrontFrame(
    CameraFrame_t *front_frame)
{
    if ((front_frame == NULL) ||
        (Camera_FrameBuffer_GetFrontFrame(front_frame) != CAMERA_FB_OK) ||
        (front_frame->data == NULL) ||
        (front_frame->width != CAMERA_FB_WIDTH) ||
        (front_frame->height != CAMERA_FB_HEIGHT) ||
        (front_frame->size_bytes != CAMERA_FB_SIZE_BYTES) ||
        (front_frame->size_bytes !=
         ((uint32_t)front_frame->width *
          (uint32_t)front_frame->height *
          CAMERA_FB_BYTES_PER_PIXEL)))
    {
        return CAMERA_SD_ERR_FRAME_BUFFER_INVALID;
    }

    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_StageFrontFrame(
    uint32_t *source_nonzero,
    uint32_t *source_sum32)
{
    CameraFrame_t front_frame;
    uint32_t nonzero_count = 0U;
    uint32_t sum32 = 0U;
    uint32_t index;
    uint32_t result;

    if ((source_nonzero == NULL) || (source_sum32 == NULL))
    {
        return CAMERA_SD_ERR_INVALID_ARGUMENT;
    }

    result = Camera_SDStorage_GetValidatedFrontFrame(&front_frame);
    if (result != CAMERA_SD_OK)
    {
        return result;
    }

    (void)memcpy(
        s_camera_sd_snapshot_image_buffer,
        front_frame.data,
        CAMERA_FB_SIZE_BYTES);

    for (index = 0U; index < CAMERA_FB_SIZE_BYTES; ++index)
    {
        uint8_t value = s_camera_sd_snapshot_image_buffer[index];

        sum32 += (uint32_t)value;
        if (value != 0U)
        {
            ++nonzero_count;
        }
    }

    *source_nonzero = nonzero_count;
    *source_sum32 = sum32;
    if ((nonzero_count == 0U) || (sum32 == 0U))
    {
        return CAMERA_SD_ERR_FRAME_EMPTY;
    }

    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_PrepareAndStageFrontFrame(
    CameraSdSnapshotResult_t *result_info)
{
    uint32_t prepare_result;
    uint32_t stage_result;
    uint32_t retry_count;

    if (result_info == NULL)
    {
        return CAMERA_SD_ERR_INVALID_ARGUMENT;
    }

    result_info->prepare_text = "FAIL";
    for (retry_count = 0U;
         retry_count <= CAMERA_SD_FRAME_PREPARE_MAX_RETRIES;
         ++retry_count)
    {
        result_info->prepare_retry = retry_count;
        result_info->source_nonzero = 0U;
        result_info->source_sum32 = 0U;
        prepare_result = Camera_RTOS_PrepareRgb565Frame(
            CAMERA_RTOS_RGB565_PREPARE_TIMEOUT_MS);
        if (prepare_result != CAMERA_RTOS_ERR_NONE)
        {
            return (prepare_result == CAMERA_RTOS_ERR_SNAPSHOT_TIMEOUT) ?
                CAMERA_SD_ERR_FRAME_PREPARE_TIMEOUT :
                CAMERA_SD_ERR_FRAME_PREPARE_FAILED;
        }

        stage_result = Camera_SDStorage_StageFrontFrame(
            &result_info->source_nonzero,
            &result_info->source_sum32);
        if (stage_result == CAMERA_SD_OK)
        {
            result_info->prepare_text = "PASS";
            return CAMERA_SD_OK;
        }
        if (stage_result != CAMERA_SD_ERR_FRAME_EMPTY)
        {
            return stage_result;
        }

        if (retry_count < CAMERA_SD_FRAME_PREPARE_MAX_RETRIES)
        {
            HAL_Delay(CAMERA_SD_FRAME_PREPARE_RETRY_DELAY_MS);
        }
    }

    return CAMERA_SD_ERR_FRAME_EMPTY;
}

uint32_t Camera_SDStorage_SaveSnapshotFrame(
    CameraSdSnapshotResult_t *snapshot_result)
{
    CameraSdSnapshotResult_t result_info;
    uint32_t result = CAMERA_SD_OK;
    uint32_t cleanup_result = CAMERA_SD_OK;
    uint32_t restore_result = CAMERA_SD_OK;
    uint32_t step_result;
    uint32_t camera_restore_required = 0U;
    uint32_t dvp_restore_required = 0U;
    uint32_t takeover_attempted = 0U;
    uint32_t mount_attempted = 0U;
    uint32_t file_opened = 0U;
    uint32_t restore_continuous_capture = 0U;
    FRESULT fatfs_result;
    UINT bytes_written = 0U;

    memset(&result_info, 0, sizeof(result_info));
    result_info.file_name = CAMERA_SD_SNAPSHOT_FILE_NAME;
    result_info.source_text = CAMERA_SD_SNAPSHOT_SOURCE_TEXT;
    result_info.source_bytes = CAMERA_FB_SIZE_BYTES;
    result_info.prepare_text = "NOT_RUN";
    result_info.format_text = CAMERA_SD_SNAPSHOT_FORMAT_TEXT;
    result_info.width = CAMERA_FB_WIDTH;
    result_info.height = CAMERA_FB_HEIGHT;
    result_info.mount_text = "NOT_RUN";
    result_info.write_text = "NOT_RUN";
    result_info.cleanup_text = "PASS";
    result_info.restore_text = "PASS";
    result_info.error_code = CAMERA_SD_OK;
    result_info.error_text = Camera_SDStorage_ErrorToString(CAMERA_SD_OK);

    s_camera_sd_status.last_snapshot_text = "RUNNING";
    s_camera_sd_status.last_file_name = CAMERA_SD_SNAPSHOT_FILE_NAME;
    s_camera_sd_status.last_file_size = 0U;
    s_camera_sd_status.last_mount_result =
        CAMERA_SD_FATFS_MOUNT_NOT_RUN;
    s_camera_sd_status.last_mount_text = "NOT_RUN";
    s_camera_sd_status.card_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_fatfs_disk_error = CAMERA_SD_OK;
    Camera_SDStorage_SetSaveError(CAMERA_SD_OK);
    Camera_SDStorage_SetLastError(CAMERA_SD_OK);

    if ((Camera_SnapshotControl_IsSoftwareGuardActive() != 0U) ||
        (s_camera_sd_fatfs_session_active != 0U) ||
        (s_camera_sd_full_gpio_af12_selected != 0U) ||
        (s_camera_sd_clock_enabled != 0U) ||
        (s_camera_sd_status.dvp_mask_active != 0U))
    {
        result = CAMERA_SD_ERR_SNAPSHOT_BUSY;
        s_camera_sd_status.last_snapshot_text = "FAIL";
        Camera_SDStorage_SetSaveError(result);
        Camera_SDStorage_SetLastError(result);
        result_info.error_code = result;
        result_info.error_text = Camera_SDStorage_ErrorToString(result);
        if (snapshot_result != NULL)
        {
            *snapshot_result = result_info;
        }
        return result;
    }

    s_camera_sd_status.dvp_reg_3018_saved =
        CAMERA_SD_REG_VALUE_UNKNOWN;
    restore_continuous_capture =
        ((DCMI->CR & DCMI_CR_CAPTURE) != 0U) ? 1U : 0U;
    Camera_SDStorage_EnsureDcmiDmaHandle();
    camera_restore_required = 1U;
    step_result = Camera_SDStorage_PrepareAndStageFrontFrame(&result_info);
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    step_result = Camera_SnapshotControl_RequestPrepare();
    if (step_result != CAMERA_SNAPSHOT_OK)
    {
        result = CAMERA_SD_ERR_SNAPSHOT_PREPARE_FAILED;
        goto cleanup;
    }

    step_result = Camera_SDStorage_StopDvpConflictPads();
    if (s_camera_sd_status.dvp_reg_3018_saved !=
        CAMERA_SD_REG_VALUE_UNKNOWN)
    {
        dvp_restore_required = 1U;
    }
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    takeover_attempted = 1U;
    step_result = Camera_SDStorage_RequestTakeoverEnter();
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    step_result = Camera_SDStorage_RequestInit();
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    s_camera_sd_fatfs_session_active = 1U;
    mount_attempted = 1U;
    fatfs_result = f_mount(&s_camera_sd_fatfs, "", 1U);
    Camera_SDStorage_SetMountResult(fatfs_result);
    result_info.mount_text = s_camera_sd_status.last_mount_text;
    if (fatfs_result != FR_OK)
    {
        result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_MOUNT_FAILED;
        goto cleanup;
    }

    s_camera_sd_status.fatfs_ready = 1U;
    s_camera_sd_fatfs_write_allowed = 1U;
    fatfs_result = f_open(
        &s_camera_sd_file,
        CAMERA_SD_SNAPSHOT_FILE_NAME,
        FA_CREATE_ALWAYS | FA_WRITE);
    if (fatfs_result != FR_OK)
    {
        result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_FILE_OPEN_FAILED;
        goto cleanup;
    }
    file_opened = 1U;

    fatfs_result = f_write(
        &s_camera_sd_file,
        s_camera_sd_snapshot_image_buffer,
        (UINT)CAMERA_FB_SIZE_BYTES,
        &bytes_written);
    if ((fatfs_result != FR_OK) ||
        (bytes_written != (UINT)CAMERA_FB_SIZE_BYTES))
    {
        result_info.write_text = "FAIL";
        result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED;
        goto cleanup;
    }
    result_info.write_text = "PASS";

cleanup:
    if (file_opened != 0U)
    {
        fatfs_result = f_close(&s_camera_sd_file);
        file_opened = 0U;
        if (fatfs_result != FR_OK)
        {
            result_info.write_text = "FAIL";
            step_result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
                s_camera_sd_fatfs_disk_error :
                CAMERA_SD_ERR_FATFS_FILE_CLOSE_FAILED;
            Camera_SDStorage_RecordFirstError(&cleanup_result, step_result);
            Camera_SDStorage_RecordFirstError(&result, step_result);
        }
    }
    s_camera_sd_fatfs_write_allowed = 0U;

    if (mount_attempted != 0U)
    {
        fatfs_result = f_mount(NULL, "", 0U);
        if (fatfs_result != FR_OK)
        {
            step_result = CAMERA_SD_ERR_FATFS_UNMOUNT_FAILED;
            Camera_SDStorage_RecordFirstError(&cleanup_result, step_result);
            Camera_SDStorage_RecordFirstError(&result, step_result);
        }
        s_camera_sd_status.fatfs_ready = 0U;
    }
    s_camera_sd_fatfs_session_active = 0U;

    if (takeover_attempted != 0U)
    {
        step_result = Camera_SDStorage_RequestTakeoverExit();
        Camera_SDStorage_RecordFirstError(&restore_result, step_result);
        Camera_SDStorage_RecordFirstError(&cleanup_result, step_result);
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    if (dvp_restore_required != 0U)
    {
        step_result = Camera_SDStorage_RestoreDvpConflictPads();
        Camera_SDStorage_RecordFirstError(&restore_result, step_result);
        Camera_SDStorage_RecordFirstError(&cleanup_result, step_result);
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    if (camera_restore_required != 0U)
    {
        step_result = Camera_SDStorage_RestoreCameraLink(
            restore_continuous_capture,
            takeover_attempted);
        Camera_SDStorage_RecordFirstError(&restore_result, step_result);
        Camera_SDStorage_RecordFirstError(&cleanup_result, step_result);
        Camera_SDStorage_RecordFirstError(&result, step_result);
    }

    result_info.restore_text =
        (restore_result == CAMERA_SD_OK) ? "PASS" : "FAIL";
    result_info.cleanup_text =
        (cleanup_result == CAMERA_SD_OK) ? "PASS" : "FAIL";
    if (result == CAMERA_SD_OK)
    {
        s_camera_sd_status.last_snapshot_text = "PASS";
        s_camera_sd_status.last_file_size = (uint32_t)bytes_written;
        ++s_camera_sd_status.save_count;
    }
    else
    {
        s_camera_sd_status.last_snapshot_text = "FAIL";
        s_camera_sd_status.last_file_size = 0U;
    }

    s_camera_sd_status.card_ready =
        (result == CAMERA_SD_OK) ? 1U : 0U;
    Camera_SDStorage_SetSaveError(result);
    Camera_SDStorage_SetLastError(result);
    result_info.bytes_written = s_camera_sd_status.last_file_size;
    result_info.error_code = result;
    result_info.error_text = Camera_SDStorage_ErrorToString(result);
    if (snapshot_result != NULL)
    {
        *snapshot_result = result_info;
    }
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
        case CAMERA_SD_ERR_SNAPSHOT_BUSY:
            return "SNAPSHOT_BUSY";
        case CAMERA_SD_ERR_SNAPSHOT_PREPARE_FAILED:
            return "SNAPSHOT_PREPARE_FAILED";
        case CAMERA_SD_ERR_FATFS_MOUNT_FAILED:
            return "FATFS_MOUNT_FAILED";
        case CAMERA_SD_ERR_FATFS_UNMOUNT_FAILED:
            return "FATFS_UNMOUNT_FAILED";
        case CAMERA_SD_ERR_FATFS_DISK_NOT_READY:
            return "FATFS_DISK_NOT_READY";
        case CAMERA_SD_ERR_FATFS_DISK_READ_FAILED:
            return "FATFS_DISK_READ_FAILED";
        case CAMERA_SD_ERR_FATFS_DISK_IOCTL_FAILED:
            return "FATFS_DISK_IOCTL_FAILED";
        case CAMERA_SD_ERR_FATFS_CARD_TIMEOUT:
            return "FATFS_CARD_TIMEOUT";
        case CAMERA_SD_ERR_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case CAMERA_SD_ERR_FATFS_DISK_WRITE_FAILED:
            return "FATFS_DISK_WRITE_FAILED";
        case CAMERA_SD_ERR_FATFS_FILE_OPEN_FAILED:
            return "FATFS_FILE_OPEN_FAILED";
        case CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED:
            return "FATFS_FILE_WRITE_FAILED";
        case CAMERA_SD_ERR_FATFS_FILE_CLOSE_FAILED:
            return "FATFS_FILE_CLOSE_FAILED";
        case CAMERA_SD_ERR_SDIO_CLOCK_DISABLE_FAILED:
            return "SDIO_CLOCK_DISABLE_FAILED";
        case CAMERA_SD_ERR_CAMERA_RESTORE_FAILED:
            return "CAMERA_RESTORE_FAILED";
        case CAMERA_SD_ERR_FRAME_BUFFER_INVALID:
            return "FRAME_BUFFER_INVALID";
        case CAMERA_SD_ERR_FRAME_EMPTY:
            return "FRAME_EMPTY";
        case CAMERA_SD_ERR_FRAME_PREPARE_FAILED:
            return "FRAME_PREPARE_FAILED";
        case CAMERA_SD_ERR_FRAME_PREPARE_TIMEOUT:
            return "FRAME_PREPARE_TIMEOUT";
        default:
            return "UNKNOWN_ERROR";
    }
}
