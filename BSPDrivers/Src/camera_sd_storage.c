#include "camera_sd_storage.h"

#include "camera_snapshot_control.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>

/*
 * Stage 11C-1 在既有冲突引脚切换成功后，将 PC8～PC12 和 PD2 配置为 SDIO AF12，
 * 并在 EXIT 时先将六个引脚退回 GPIO 输入态，再恢复 PC8、PC9、PC11 的 DCMI AF13。
 * PC8、PC9、PC11 同时被 DCMI 和 SDIO 使用，后续必须先停止 DCMI 和相关 DMA，
 * 再进入 SDIO 接管流程。Stage 11C-3 只在完整 AF12 状态下验证 HAL_SD_Init，
 * 不接入 FATFS，不执行块读写，也不启用 SDIO 中断或 DMA。
 */
static CameraSdStorageStatus_t s_camera_sd_status;
static SD_HandleTypeDef hsd_snapshot;

#define CAMERA_SD_CONFLICT_PIN_MASK \
    (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11)
#define CAMERA_SD_CONFLICT_MODE_MASK \
    ((3UL << (8U * 2U)) | (3UL << (9U * 2U)) | (3UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_AF_MODE \
    ((2UL << (8U * 2U)) | (2UL << (9U * 2U)) | (2UL << (11U * 2U)))
#define CAMERA_SD_CONFLICT_SPEED_VERY_HIGH \
    CAMERA_SD_CONFLICT_MODE_MASK
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
#define CAMERA_SD_FULL_GPIOC_AF_MODE \
    ((2UL << (8U * 2U)) | (2UL << (9U * 2U)) | \
     (2UL << (10U * 2U)) | (2UL << (11U * 2U)) | \
     (2UL << (12U * 2U)))
#define CAMERA_SD_FULL_GPIOC_PULLUP \
    ((1UL << (8U * 2U)) | (1UL << (9U * 2U)) | \
     (1UL << (10U * 2U)) | (1UL << (11U * 2U)) | \
     (1UL << (12U * 2U)))
#define CAMERA_SD_FULL_GPIOC_AFRH_MASK \
    ((0xFUL << 0U) | (0xFUL << 4U) | (0xFUL << 8U) | \
     (0xFUL << 12U) | (0xFUL << 16U))
#define CAMERA_SD_FULL_GPIOC_AFRH_AF12 \
    ((12UL << 0U) | (12UL << 4U) | (12UL << 8U) | \
     (12UL << 12U) | (12UL << 16U))

#define CAMERA_SD_FULL_GPIOD_PIN_MASK GPIO_PIN_2
#define CAMERA_SD_FULL_GPIOD_MODE_MASK (3UL << (2U * 2U))
#define CAMERA_SD_FULL_GPIOD_AF_MODE (2UL << (2U * 2U))
#define CAMERA_SD_FULL_GPIOD_PULLUP (1UL << (2U * 2U))
#define CAMERA_SD_FULL_GPIOD_AFRL_MASK (0xFUL << 8U)
#define CAMERA_SD_FULL_GPIOD_AFRL_AF12 (12UL << 8U)

static void Camera_SDStorage_PrepareSdHandle(void)
{
    hsd_snapshot.Instance = SDIO;
    hsd_snapshot.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    hsd_snapshot.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    hsd_snapshot.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    /* 本轮只验证 1-bit 初始化，不切换 4-bit，也不调用 HAL_SD_ConfigWideBusOperation。 */
    hsd_snapshot.Init.BusWide = SDIO_BUS_WIDE_1B;
    hsd_snapshot.Init.HardwareFlowControl =
        SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    /* ClockDiv=118U 用于保守的 SDIO 初始化低速阶段。 */
    hsd_snapshot.Init.ClockDiv = 118U;
    /* Stage 11C-3 只验证 HAL_SD_Init 调用路径，不接 FATFS，不执行块读写。 */
}

static void Camera_SDStorage_EnableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_ENABLE();
    s_camera_sd_status.sdio_clock_enabled = 1U;
}

static void Camera_SDStorage_DisableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_DISABLE();
    s_camera_sd_status.sdio_clock_enabled = 0U;
}

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

static uint32_t Camera_SDStorage_SwitchConflictPinsToSdioAf12(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.sdio_af12_switch_attempt_count;

    /* 只将 PC8、PC9、PC11 切换为 SDIO AF12，不处理 PC10、PC12、PD2。 */
    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* 进入复用态后已不再是 GPIO 输入释放态。 */
    s_camera_sd_status.conflict_pins_released = 0U;

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
        ++s_camera_sd_status.sdio_af12_switch_error_count;
        s_camera_sd_status.sdio_af12_selected = 0U;
        s_camera_sd_status.last_sdio_af12_error_code =
            CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED;
        s_camera_sd_status.last_sdio_af12_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED;
    }

    ++s_camera_sd_status.sdio_af12_switch_success_count;
    s_camera_sd_status.sdio_af12_selected = 1U;
    s_camera_sd_status.last_sdio_af12_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_af12_operation_ms =
        HAL_GetTick() - start_ms;
    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_LeaveSdioAf12ToInput(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.sdio_af12_restore_attempt_count;

    /* 只将 PC8、PC9、PC11 从 SDIO AF12 退回无上下拉的 GPIO 输入态。 */
    gpio.Pin = CAMERA_SD_CONFLICT_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_CONFLICT_MODE_MASK) != 0U) ||
        ((GPIOC->PUPDR & CAMERA_SD_CONFLICT_MODE_MASK) != 0U))
    {
        ++s_camera_sd_status.sdio_af12_restore_error_count;
        s_camera_sd_status.sdio_af12_selected = 0U;
        s_camera_sd_status.conflict_pins_released = 0U;
        s_camera_sd_status.last_sdio_af12_error_code =
            CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED;
        s_camera_sd_status.last_sdio_af12_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED;
    }

    ++s_camera_sd_status.sdio_af12_restore_success_count;
    s_camera_sd_status.sdio_af12_selected = 0U;
    s_camera_sd_status.conflict_pins_released = 1U;
    s_camera_sd_status.last_sdio_af12_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_af12_operation_ms =
        HAL_GetTick() - start_ms;
    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_SwitchFullSdioGpioToAf12(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.sdio_full_gpio_switch_attempt_count;

    /* 配置 PC8、PC9、PC10、PC11、PC12 为完整 SDIO GPIO 的 AF12。 */
    gpio.Pin = CAMERA_SD_FULL_GPIOC_PIN_MASK;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* 配置 PD2 为 SDIO_CMD 的 AF12，不访问 SDIO 外设。 */
    gpio.Pin = CAMERA_SD_FULL_GPIOD_PIN_MASK;
    HAL_GPIO_Init(GPIOD, &gpio);

    if (((GPIOC->MODER & CAMERA_SD_FULL_GPIOC_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOC_AF_MODE) ||
        ((GPIOC->PUPDR & CAMERA_SD_FULL_GPIOC_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOC_PULLUP) ||
        ((GPIOC->OSPEEDR & CAMERA_SD_FULL_GPIOC_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOC_MODE_MASK) ||
        ((GPIOC->OTYPER & CAMERA_SD_FULL_GPIOC_PIN_MASK) != 0U) ||
        ((GPIOC->AFR[1] & CAMERA_SD_FULL_GPIOC_AFRH_MASK) !=
         CAMERA_SD_FULL_GPIOC_AFRH_AF12) ||
        ((GPIOD->MODER & CAMERA_SD_FULL_GPIOD_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOD_AF_MODE) ||
        ((GPIOD->PUPDR & CAMERA_SD_FULL_GPIOD_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOD_PULLUP) ||
        ((GPIOD->OSPEEDR & CAMERA_SD_FULL_GPIOD_MODE_MASK) !=
         CAMERA_SD_FULL_GPIOD_MODE_MASK) ||
        ((GPIOD->OTYPER & CAMERA_SD_FULL_GPIOD_PIN_MASK) != 0U) ||
        ((GPIOD->AFR[0] & CAMERA_SD_FULL_GPIOD_AFRL_MASK) !=
         CAMERA_SD_FULL_GPIOD_AFRL_AF12))
    {
        ++s_camera_sd_status.sdio_full_gpio_switch_error_count;
        s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
        s_camera_sd_status.last_sdio_full_gpio_error_code =
            CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
        s_camera_sd_status.last_sdio_full_gpio_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
    }

    ++s_camera_sd_status.sdio_full_gpio_switch_success_count;
    s_camera_sd_status.sdio_full_gpio_af12_selected = 1U;
    s_camera_sd_status.last_sdio_full_gpio_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_full_gpio_operation_ms =
        HAL_GetTick() - start_ms;
    return CAMERA_SD_OK;
}

static uint32_t Camera_SDStorage_LeaveFullSdioGpioToInput(void)
{
    uint32_t start_ms = HAL_GetTick();
    GPIO_InitTypeDef gpio = {0};

    ++s_camera_sd_status.sdio_full_gpio_restore_attempt_count;

    /* 将 PC8～PC12 全部退回无上下拉的 GPIO 输入态。 */
    gpio.Pin = CAMERA_SD_FULL_GPIOC_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* 将 PD2 同步退回 GPIO 输入态。 */
    gpio.Pin = CAMERA_SD_FULL_GPIOD_PIN_MASK;
    HAL_GPIO_Init(GPIOD, &gpio);

    s_camera_sd_status.sdio_af12_selected = 0U;
    s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;

    if (((GPIOC->MODER & CAMERA_SD_FULL_GPIOC_MODE_MASK) != 0U) ||
        ((GPIOC->PUPDR & CAMERA_SD_FULL_GPIOC_MODE_MASK) != 0U) ||
        ((GPIOD->MODER & CAMERA_SD_FULL_GPIOD_MODE_MASK) != 0U) ||
        ((GPIOD->PUPDR & CAMERA_SD_FULL_GPIOD_MODE_MASK) != 0U))
    {
        ++s_camera_sd_status.sdio_full_gpio_restore_error_count;
        s_camera_sd_status.conflict_pins_released = 0U;
        s_camera_sd_status.last_sdio_full_gpio_error_code =
            CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED;
        s_camera_sd_status.last_sdio_full_gpio_operation_ms =
            HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED;
    }

    ++s_camera_sd_status.sdio_full_gpio_restore_success_count;
    s_camera_sd_status.conflict_pins_released = 1U;
    s_camera_sd_status.last_sdio_full_gpio_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_full_gpio_operation_ms =
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
    s_camera_sd_status.sdio_af12_switch_attempt_count = 0U;
    s_camera_sd_status.sdio_af12_switch_success_count = 0U;
    s_camera_sd_status.sdio_af12_switch_error_count = 0U;
    s_camera_sd_status.sdio_af12_restore_attempt_count = 0U;
    s_camera_sd_status.sdio_af12_restore_success_count = 0U;
    s_camera_sd_status.sdio_af12_restore_error_count = 0U;
    s_camera_sd_status.sdio_af12_selected = 0U;
    s_camera_sd_status.last_sdio_af12_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_af12_operation_ms = 0U;
    s_camera_sd_status.sdio_full_gpio_switch_attempt_count = 0U;
    s_camera_sd_status.sdio_full_gpio_switch_success_count = 0U;
    s_camera_sd_status.sdio_full_gpio_switch_error_count = 0U;
    s_camera_sd_status.sdio_full_gpio_restore_attempt_count = 0U;
    s_camera_sd_status.sdio_full_gpio_restore_success_count = 0U;
    s_camera_sd_status.sdio_full_gpio_restore_error_count = 0U;
    s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
    s_camera_sd_status.last_sdio_full_gpio_error_code = CAMERA_SD_OK;
    s_camera_sd_status.last_sdio_full_gpio_operation_ms = 0U;
    s_camera_sd_status.real_hal_sd_init_enabled = 1U;
    s_camera_sd_status.sdio_clock_enabled = 0U;
    s_camera_sd_status.sdio_hal_init_attempt_count = 0U;
    s_camera_sd_status.sdio_hal_init_success_count = 0U;
    s_camera_sd_status.sdio_hal_init_error_count = 0U;
    s_camera_sd_status.sdio_hal_deinit_attempt_count = 0U;
    s_camera_sd_status.sdio_hal_deinit_success_count = 0U;
    s_camera_sd_status.sdio_hal_deinit_error_count = 0U;
    s_camera_sd_status.last_hal_sd_init_status = (uint32_t)HAL_OK;
    s_camera_sd_status.last_hal_sd_deinit_status = (uint32_t)HAL_OK;
    s_camera_sd_status.last_hal_sd_error = HAL_SD_ERROR_NONE;
    s_camera_sd_status.last_sdio_hal_init_operation_ms = 0U;
    s_camera_sd_status.last_sdio_hal_deinit_operation_ms = 0U;
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
    uint32_t start_ms = HAL_GetTick();
    uint32_t hal_start_ms;
    HAL_StatusTypeDef hal_status;

    ++s_camera_sd_status.init_attempt_count;

    if (s_camera_sd_status.sdio_full_gpio_af12_selected != 1U)
    {
        /* 完整 SDIO GPIO 未接管时只返回 NEED_TAKEOVER，不调用 HAL_SD_Init。 */
        s_camera_sd_status.last_error_code = CAMERA_SD_ERR_NEED_TAKEOVER;
        s_camera_sd_status.is_initialized = 0U;
        s_camera_sd_status.takeover_required = 1U;
        s_camera_sd_status.sdio_ready = 0U;
        s_camera_sd_status.fatfs_ready = 0U;
        s_camera_sd_status.last_operation_ms = HAL_GetTick() - start_ms;
        return CAMERA_SD_ERR_NEED_TAKEOVER;
    }

    Camera_SDStorage_PrepareSdHandle();
    Camera_SDStorage_EnableSdioClock();
    ++s_camera_sd_status.sdio_hal_init_attempt_count;

    hal_start_ms = HAL_GetTick();
    hal_status = HAL_SD_Init(&hsd_snapshot);
    s_camera_sd_status.last_sdio_hal_init_operation_ms =
        HAL_GetTick() - hal_start_ms;
    s_camera_sd_status.last_hal_sd_init_status = (uint32_t)hal_status;
    s_camera_sd_status.last_hal_sd_error = HAL_SD_GetError(&hsd_snapshot);
    s_camera_sd_status.fatfs_ready = 0U;

    if (hal_status == HAL_OK)
    {
        ++s_camera_sd_status.init_success_count;
        ++s_camera_sd_status.sdio_hal_init_success_count;
        s_camera_sd_status.is_initialized = 1U;
        s_camera_sd_status.sdio_ready = 1U;
        s_camera_sd_status.last_error_code = CAMERA_SD_OK;
        s_camera_sd_status.last_operation_ms = HAL_GetTick() - start_ms;
        return CAMERA_SD_OK;
    }

    ++s_camera_sd_status.init_error_count;
    ++s_camera_sd_status.sdio_hal_init_error_count;
    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.last_error_code = CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED;
    s_camera_sd_status.last_operation_ms = HAL_GetTick() - start_ms;
    return CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED;
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
        s_camera_sd_status.conflict_pins_released = 0U;
        s_camera_sd_status.sdio_af12_selected = 0U;
        s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
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
            uint32_t af12_result;

            af12_result = Camera_SDStorage_SwitchConflictPinsToSdioAf12();
            if (af12_result == CAMERA_SD_OK)
            {
                uint32_t full_gpio_result;

                full_gpio_result =
                    Camera_SDStorage_SwitchFullSdioGpioToAf12();
                if (full_gpio_result == CAMERA_SD_OK)
                {
                    /* 六个 SDIO GPIO 已切换到 AF12，但不初始化 SDIO 外设。 */
                    s_camera_sd_status.sdio_af12_selected = 1U;
                    s_camera_sd_status.sdio_full_gpio_af12_selected = 1U;
                    s_camera_sd_status.conflict_pins_released = 0U;
                    s_camera_sd_status.takeover_state =
                        CAMERA_SD_TAKEOVER_STATE_ENTER_DEFERRED;
                    s_camera_sd_status.last_conflict_pin_error_code =
                        CAMERA_SD_OK;
                    s_camera_sd_status.last_sdio_af12_error_code = CAMERA_SD_OK;
                    s_camera_sd_status.last_sdio_full_gpio_error_code =
                        CAMERA_SD_OK;
                    s_camera_sd_status.last_takeover_precheck_error_code =
                        CAMERA_SD_OK;
                    s_camera_sd_status.last_takeover_error_code =
                        CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
                    result = CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED;
                }
                else
                {
                    ++s_camera_sd_status.takeover_error_count;
                    s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
                    s_camera_sd_status.takeover_state =
                        CAMERA_SD_TAKEOVER_STATE_ERROR;
                    s_camera_sd_status.last_takeover_error_code =
                        CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
                    result = CAMERA_SD_ERR_SDIO_FULL_GPIO_SWITCH_FAILED;
                }
            }
            else
            {
                ++s_camera_sd_status.takeover_error_count;
                s_camera_sd_status.sdio_af12_selected = 0U;
                s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
                s_camera_sd_status.takeover_state =
                    CAMERA_SD_TAKEOVER_STATE_ERROR;
                s_camera_sd_status.last_takeover_error_code =
                    CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED;
                result = CAMERA_SD_ERR_SDIO_AF12_SWITCH_FAILED;
            }
        }
        else
        {
            ++s_camera_sd_status.takeover_error_count;
            s_camera_sd_status.conflict_pin_release_ready = 0U;
            s_camera_sd_status.sdio_af12_selected = 0U;
            s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
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
    s_camera_sd_status.conflict_pin_release_ready = 0U;

    if ((s_camera_sd_status.is_initialized != 0U) ||
        (s_camera_sd_status.sdio_clock_enabled != 0U))
    {
        uint32_t hal_start_ms = HAL_GetTick();
        HAL_StatusTypeDef hal_status;

        ++s_camera_sd_status.sdio_hal_deinit_attempt_count;
        hal_status = HAL_SD_DeInit(&hsd_snapshot);
        s_camera_sd_status.last_sdio_hal_deinit_operation_ms =
            HAL_GetTick() - hal_start_ms;
        s_camera_sd_status.last_hal_sd_deinit_status = (uint32_t)hal_status;
        s_camera_sd_status.last_hal_sd_error = HAL_SD_GetError(&hsd_snapshot);

        if (hal_status == HAL_OK)
        {
            ++s_camera_sd_status.sdio_hal_deinit_success_count;
        }
        else
        {
            ++s_camera_sd_status.sdio_hal_deinit_error_count;
            s_camera_sd_status.last_error_code =
                CAMERA_SD_ERR_SDIO_HAL_DEINIT_FAILED;
        }

        /* 即使 HAL_SD_DeInit 失败，也必须关时钟并继续恢复 GPIO。 */
        Camera_SDStorage_DisableSdioClock();
    }

    s_camera_sd_status.is_initialized = 0U;
    s_camera_sd_status.sdio_ready = 0U;
    s_camera_sd_status.fatfs_ready = 0U;

    if (s_camera_sd_status.sdio_full_gpio_af12_selected != 0U)
    {
        uint32_t full_gpio_result =
            Camera_SDStorage_LeaveFullSdioGpioToInput();

        if (full_gpio_result != CAMERA_SD_OK)
        {
            ++s_camera_sd_status.takeover_error_count;
            s_camera_sd_status.sdio_af12_selected = 0U;
            s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_ERROR;
            s_camera_sd_status.last_takeover_error_code =
                CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED;
            s_camera_sd_status.last_takeover_operation_ms =
                HAL_GetTick() - start_ms;
            return CAMERA_SD_ERR_SDIO_FULL_GPIO_RESTORE_FAILED;
        }
    }
    else if (s_camera_sd_status.sdio_af12_selected != 0U)
    {
        uint32_t af12_result = Camera_SDStorage_LeaveSdioAf12ToInput();

        if (af12_result != CAMERA_SD_OK)
        {
            ++s_camera_sd_status.takeover_error_count;
            s_camera_sd_status.takeover_state = CAMERA_SD_TAKEOVER_STATE_ERROR;
            s_camera_sd_status.last_takeover_error_code =
                CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED;
            s_camera_sd_status.last_takeover_operation_ms =
                HAL_GetTick() - start_ms;
            return CAMERA_SD_ERR_SDIO_AF12_RESTORE_FAILED;
        }
    }

    pin_result = Camera_SDStorage_RestoreConflictPins();

    if (pin_result == CAMERA_SD_OK)
    {
        s_camera_sd_status.conflict_pins_released = 0U;
        s_camera_sd_status.sdio_af12_selected = 0U;
        s_camera_sd_status.sdio_full_gpio_af12_selected = 0U;
        s_camera_sd_status.snapshot_pause_confirmed = 0U;
        s_camera_sd_status.takeover_state =
            CAMERA_SD_TAKEOVER_STATE_EXIT_DEFERRED;
        s_camera_sd_status.last_conflict_pin_error_code = CAMERA_SD_OK;
        s_camera_sd_status.last_sdio_af12_error_code = CAMERA_SD_OK;
        s_camera_sd_status.last_sdio_full_gpio_error_code = CAMERA_SD_OK;
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
