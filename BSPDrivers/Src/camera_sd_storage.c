#include "camera_sd_storage.h"

#include "camera_snapshot_control.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>

/*
 * Stage 11B-9 在前置检查通过后释放 PC8、PC9、PC11，并在 EXIT 时恢复 AF13。
 * PC8、PC9、PC11 同时被 DCMI 和 SDIO 使用，后续必须先停止 DCMI 和相关 DMA，
 * 再进入 SDIO 接管流程；本文件不配置 AF12，也不初始化 SDIO 或 FATFS。
 */
static CameraSdStorageStatus_t s_camera_sd_status;

#define CAMERA_SD_CONFLICT_PIN_MASK \
    (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11)
#define CAMERA_SD_CONFLICT_MODE_MASK \
    ((3UL << (8U * 2U)) | (3UL << (9U * 2U)) | (3UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_AF_MODE \
    ((2UL << (8U * 2U)) | (2UL << (9U * 2U)) | (2UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_SPEED_VERY_HIGH \
    CAMERA_SD_CONFLICT_MODE_MASK
#define CAMERA_SD_CONFLICT_AFRH_MASK \
    ((0xFUL << 0U) | (0xFUL << 4U) | (0xFUL << 12U))
#define CAMERA_SD_CONFLICT_AFRH_AF13 \
    ((13UL << 0U) | (13UL << 4U) | (13UL << 12U))

static uint32_t Camera_SDStorage_ReleaseConflictPins(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.conflict_pin_release_attempt_count;

    /* 只释放 PC8、PC9、PC11；不配置 PC10、PC12、PD2 或任何 SDIO 复用。 */
    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) != 0U) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) != 0U))
    {
        ++s_camera_sd_status.conflict_pin_release_error_count;
        s_camera_sd_status.conflict_pins_released = 0U;
        s_camera_sd_status.last_conflict_pin_error_code =
            CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED;
        s_camera_sd_status.last_conflict_pin_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED;
    }

    ++s_camera_sd_status.conflict_pin_release_success_count;
    s_camera_sd_status.conflict_pins_released = 1U;
    s_camera_sd_status.last_conflict_pin_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_conflict_pin_operation_ms =
        HAL_GetTick() - start_ms;
    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_RestoreConflictPins(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.conflict_pin_restore_attempt_count;

    /* 只把 PC8、PC9、PC11 恢复为 DCMI AF13，不重启 DCMI 或 DMA。 */
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
        ++s_camera_sd_status.conflict_pin_restore_error_count;
        s_camera_sd_status.conflict_pins_released = 1U;
        s_camera_sd_status.last_conflict_pin_error_code =
            CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED;
        s_camera_sd_status.last_conflict_pin_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED;
    }

    ++s_camera_sd_status.conflict_pin_restore_success_count;
    s_camera_sd_status.conflict_pins_released = 0U;
    s_camera_sd_status.last_conflict_pin_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_conflict_pin_operation_ms =
        HAL_GetTick() - start_ms;
    return CAMERA_SD_OK;
}

void Camera_SDStorage_InitState(void)
{
    s_camera_sd_status.init_attempt_count = 0U;
    s_camera_sd_status.init_success_count = 0U;
    s_camera_sd_status.init_error_count = 0U;
    s_camera_sd_status.last_error_code = CAMERA_SD_OK;
    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_operation_ms = 0U;
    s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_IDLE;
    s_camera_sd_status.takeover_enter_attempt_count = 0U;
    s_camera_sd_status.takeover_exit_attempt_count = 0U;
    s_camera_sd_status.takeover_enter_success_count = 0U;
    s_camera_sd_status.takeover_exit_success_count = 0U;
    s_camera_sd_status.takeover_error_count = 0U;
    s_camera_sd_status.last_takeover_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_takeover_operation_ms = 0U;
    s_camera_sd_status.takeover_precheck_required = 1U;
    s_camera_sd_status.takeover_precheck_attempt_count = 0U;
    s_camera_sd_status.takeover_precheck_success_count = 0U;
    s_camera_sd_status.takeover_precheck_fail_count = 0U;
    s_camera_sd_status.snapshot_pause_required = 1U;
    s_camera_sd_status.snapshot_pause_confirmed = 0U;
    s_camera_sd_status.conflict_pin_release_ready = 0U;
    s_camera_sd_status.last_takeover_precheck_error_code = CAMERA_SD_OK;
    s_camera_sd_status.conflict_pin_release_attempt_count = 0U;
    s_camera_sd_status.conflict_pin_release_success_count = 0U;
    s_camera_sd_status.conflict_pin_release_error_count = 0U;
    s_camera_sd_status.conflict_pin_restore_attempt_count = 0U;
    s_camera_sd_status.conflict_pin_restore_success_count = 0U;
    s_camera_sd_status.conflict_pin_restore_error_count = 0U;
    s_camera_sd_status.conflict_pins_released = 0U;
    s_camera_sd_status.last_conflict_pin_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_conflict_pin_operation_ms = 0U;
}

void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status)
{
    if (status == NULL)
    {
        return;
    }

    *status = s_camera_sd_status;
}

uint32_t Camera_SDStorage_RequestInit(void)
{
    /* 只记录受控请求，不把“等待接管”统计为 SD 卡硬件初始化失败。 */
    ++s_camera_sd_status.init_attempt_count;
    s_camera_sd_status.last_error_code = CAMERA_SD_ERR_NEED_TAKEOVER;
    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.takeover_required = 1U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;
    s_camera_sd_status.last_operation_ms = HAL_GetTick();

    return CAMERA_SD_ERR_NEED_TAKEOVER;
}

uint32_t Camera_SDStorage_RequestTakeoverEnter(void)
{
    uint32_t start_ms = HAL_GetTick();
    uint32_t result;

    ++s_camera_sd_status.takeover_enter_attempt_count;
    ++s_camera_sd_status.takeover_precheck_attempt_count;

    if (Camera_SnapshotControl_IsTakeoverPreconditionReady() == 0U)
    {
        ++s_camera_sd_status.takeover_precheck_fail_count;
        s_camera_sd_status.snapshot_pause_confirmed = 0U;
        s_camera_sd_status.conflict_pin_release_ready = 0U;
        /* 前置条件失败时保持 IDLE，避免误认为接管流程已经开始。 */
        s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_IDLE;
        s_camera_sd_status.last_takeover_error_code =
            CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
        s_camera_sd_status.last_takeover_precheck_error_code =
            CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
        result = CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED;
    }
    else
    {
        uint32_t pin_result;

        ++s_camera_sd_status.takeover_precheck_success_count;
        s_camera_sd_status.snapshot_pause_confirmed = 1U;
        s_camera_sd_status.conflict_pin_release_ready = 1U;
        s_camera_sd_status.last_takeover_precheck_error_code = CAMERA_SD_OK;

        pin_result = Camera_SDStorage_ReleaseConflictPins();
        if (pin_result == CAMERA_SD_OK)
        {
            /* 冲突引脚已释放，但本阶段仍不配置 SDIO AF12。 */
            s_camera_sd_status.takeover_state =
                CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED;
            s_camera_sd_status.last_takeover_error_code =
                CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
            result = CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
        }
        else
        {
            ++s_camera_sd_status.takeover_error_count;
            s_camera_sd_status.conflict_pin_release_ready = 0U;
            s_camera_sd_status.takeover_state =
                CAMERA_SD_TAKEOVER_STATE_ERROR;
            s_camera_sd_status.last_takeover_error_code =
                CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED;
            result = CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED;
        }
    }

    s_camera_sd_status.last_takeover_operation_ms = HAL_GetTick() - start_ms;

    return result;
}

uint32_t Camera_SDStorage_RequestTakeoverExit(void)
{
    uint32_t start_ms = HAL_GetTick();
    uint32_t pin_result;
    uint32_t result;

    ++s_camera_sd_status.takeover_exit_attempt_count;
    pin_result = Camera_SDStorage_RestoreConflictPins();
    s_camera_sd_status.conflict_pin_release_ready = 0U;

    if (pin_result == CAMERA_SD_OK)
    {
        s_camera_sd_status.snapshot_pause_confirmed = 0U;
        s_camera_sd_status.takeover_state =
            CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED;
        s_camera_sd_status.last_takeover_error_code =
            CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
        result = CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
    }
    else
    {
        ++s_camera_sd_status.takeover_error_count;
        s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_ERROR;
        s_camera_sd_status.last_takeover_error_code =
            CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED;
        result = CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED;
    }

    s_camera_sd_status.last_takeover_operation_ms = HAL_GetTick() - start_ms;

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

        case CAMERA_SD_ERR_PIN_CONFLICT:
            return "PIN_CONFLICT";

        case CAMERA_SD_ERR_NEED_TAKEOVER:
            return "NEED_TAKEOVER";

        case CAMERA_SD_ERR_INIT_FAILED:
            return "INIT_FAILED";

        case CAMERA_SD_ERR_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";

        case CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED:
            return "TAKEOVER_NOT_IMPLEMENTED";

        case CAMERA_SD_ERR_TAKEOVER_NOT_ACTIVE:
            return "TAKEOVER_NOT_ACTIVE";

        case CAMERA_SD_ERR_TAKEOVER_ALREADY_ACTIVE:
            return "TAKEOVER_ALREADY_ACTIVE";

        case CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED:
            return "SNAPSHOT_NOT_PAUSED";

        case CAMERA_SD_ERR_TAKEOVER_PRECHECK_FAILED:
            return "TAKEOVER_PRECHECK_FAILED";

        case CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED:
            return "CONFLICT_PIN_RELEASE_FAILED";

        case CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED:
            return "CONFLICT_PIN_RESTORE_FAILED";

        case CAMERA_SD_ERR_CONFLICT_PIN_NOT_RELEASED:
            return "CONFLICT_PIN_NOT_RELEASED";

        default:
            return "UNKNOWN_ERROR";
    }
}

const char *Camera_SDStorage_TakeoverStateToString(uint32_t state)
{
    switch (state)
    {
        case CAMERA_SD_TAKEOVER_STATE_IDLE:
            return "IDLE";

        case CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED:
            return "ENTER_DEFERRED";

        case CAMERA_SD_TAKEOVER_STATE_ACTIVE:
            return "ACTIVE";

        case CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED:
            return "EXIT_DEFERRED";

        case CAMERA_SD_TAKEOVER_STATE_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}
