#include "camera_sd_storage.h"

#include "bsp_log.h"
#include "camera_snapshot_control.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <string.h>

/*
 * Stage 11C-1 在既有冲突引脚切换成功后，将 PC8～PC12 和 PD2 配置为 SDIO AF12，
 * 并在 EXIT 时先将六个引脚退回 GPIO 输入态，再恢复 PC8、PC9、PC11 的 DCMI AF13。
 * PC8、PC9、PC11 同时被 DCMI 和 SDIO 使用，后续必须先停止 DCMI 和相关 DMA，
 * 再进入 SDIO 接管流程。Stage 11C-4 在 HAL_SD_Init 成功后读取 HAL 层卡信息，
 * 不接入 FATFS，不执行块读写，也不启用 SDIO 中断或 DMA。
 */
static CameraSdStorageStatus_t s_camera_sd_status;
static SD_HandleTypeDef hsd_snapshot;
/* 使用 uint32_t 数组保证 4 字节对齐，仅用于 HAL SD polling 只读块验证，不写卡。 */
static uint32_t s_sd_read_test_words[128];

/* C5G 只读保存 HAL_SD_ReadBlocks 前后的 SDIO 寄存器，不清除任何状态标志。 */
typedef struct
{
    uint32_t sta;
    uint32_t clkcr;
    uint32_t dctrl;
    uint32_t dlen;
    uint32_t dcount;
    uint32_t fifocnt;
    uint32_t power;
    uint32_t arg;
    uint32_t cmd;
    uint32_t resp_cmd;
    uint32_t resp1;
    uint32_t resp2;
    uint32_t resp3;
    uint32_t resp4;
} Camera_SDSdioRegSnapshot_t;

typedef struct
{
    Camera_SDSdioRegSnapshot_t before_wait;
    Camera_SDSdioRegSnapshot_t before_read;
    Camera_SDSdioRegSnapshot_t after_read;
    uint32_t last_hal_state_before_read;
    uint32_t last_hal_state_after_read;
    uint32_t last_hal_error_after_read;
} Camera_SDReadRegDiag_t;

static Camera_SDReadRegDiag_t s_read_reg_diag;

/* C5D-2 独立调试状态：不进入 CameraSdStorageStatus_t，也不参与 INIT/EXIT。 */
typedef struct
{
    uint32_t supported;
    uint32_t requested_width;
    uint32_t active_width;
    uint32_t attempt_count;
    uint32_t success_count;
    uint32_t error_count;
    uint32_t last_result;
    uint32_t last_hal_status;
    uint32_t last_hal_error;
    uint32_t last_pre_card_state;
    uint32_t last_post_card_state;
    uint32_t last_wait_card_state;
    uint32_t last_operation_ms;
    uint32_t last_wait_operation_ms;
    uint32_t last_wait_timeout_ms;
} Camera_SDBusWidthDebug_t;

static Camera_SDBusWidthDebug_t s_bus_width_debug = {1U};

static void Camera_SDStorage_CaptureSdioRegSnapshot(
    Camera_SDSdioRegSnapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    snapshot->sta = SDIO->STA;
    snapshot->clkcr = SDIO->CLKCR;
    snapshot->dctrl = SDIO->DCTRL;
    snapshot->dlen = SDIO->DLEN;
    snapshot->dcount = SDIO->DCOUNT;
    snapshot->fifocnt = SDIO->FIFOCNT;
    snapshot->power = SDIO->POWER;
    snapshot->arg = SDIO->ARG;
    snapshot->cmd = SDIO->CMD;
    snapshot->resp_cmd = SDIO->RESPCMD;
    snapshot->resp1 = SDIO->RESP1;
    snapshot->resp2 = SDIO->RESP2;
    snapshot->resp3 = SDIO->RESP3;
    snapshot->resp4 = SDIO->RESP4;
}

static uint32_t Camera_SDStorage_IsStaFlagSet(
    uint32_t sta,
    uint32_t flag)
{
    return ((sta & flag) != 0U) ? 1U : 0U;
}

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

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t pin_number;
    const char *name;
} Camera_SDLineDef_t;

static const Camera_SDLineDef_t s_camera_sd_lines[] =
{
    {GPIOC, 8U, "pc8_d0"},
    {GPIOC, 9U, "pc9_d1"},
    {GPIOC, 10U, "pc10_d2"},
    {GPIOC, 11U, "pc11_d3"},
    {GPIOC, 12U, "pc12_ck"},
    {GPIOD, 2U, "pd2_cmd"}
};

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

static uint32_t Camera_SDStorage_GetPinIdr(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->IDR >> pin_number) & 0x1U;
}

static void Camera_SDStorage_PrepareSdHandle(void)
{
    hsd_snapshot.Instance = SDIO;
    hsd_snapshot.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    hsd_snapshot.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    hsd_snapshot.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    /* Stage 11C-4 保持 1-bit 初始化，不切换 4-bit，也不配置宽总线。 */
    hsd_snapshot.Init.BusWide = SDIO_BUS_WIDE_1B;
    hsd_snapshot.Init.HardwareFlowControl =
        SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    /* ClockDiv=118U 用于保守的 SDIO 初始化低速阶段。 */
    hsd_snapshot.Init.ClockDiv = 118U;
    /* Stage 11C-4 读取 HAL 层卡信息，不接 FATFS，不执行块读写。 */
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

static uint32_t Camera_SDStorage_ReadCardInfo(void)
{
    uint32_t start_ms;
    HAL_StatusTypeDef hal_status;
    HAL_SD_CardInfoTypeDef card_info;

    if ((s_camera_sd_status.is_initialized != 1U) ||
        (s_camera_sd_status.sdio_ready != 1U))
    {
        /* HAL_SD_Init 未成功时禁止访问卡信息，也不增加实际读取计数。 */
        return CAMERA_SD_ERR_SDIO_HAL_INIT_FAILED;
    }

    ++s_camera_sd_status.card_info_read_attempt_count;
    start_ms = HAL_GetTick();
    hal_status = HAL_SD_GetCardInfo(&hsd_snapshot, &card_info);
    s_camera_sd_status.last_card_info_operation_ms =
        HAL_GetTick() - start_ms;
    s_camera_sd_status.last_card_info_status = (uint32_t)hal_status;
    s_camera_sd_status.last_card_info_error = HAL_SD_GetError(&hsd_snapshot);
    s_camera_sd_status.last_hal_sd_state =
        (uint32_t)HAL_SD_GetState(&hsd_snapshot);
    s_camera_sd_status.last_hal_sd_card_state =
        (uint32_t)HAL_SD_GetCardState(&hsd_snapshot);

    if (hal_status == HAL_OK)
    {
        ++s_camera_sd_status.card_info_read_success_count;
        s_camera_sd_status.card_type = card_info.CardType;
        s_camera_sd_status.card_version = card_info.CardVersion;
        s_camera_sd_status.card_class = card_info.Class;
        s_camera_sd_status.card_rel_card_add = card_info.RelCardAdd;
        s_camera_sd_status.card_block_nbr = card_info.BlockNbr;
        s_camera_sd_status.card_block_size = card_info.BlockSize;
        s_camera_sd_status.card_log_block_nbr = card_info.LogBlockNbr;
        s_camera_sd_status.card_log_block_size = card_info.LogBlockSize;
        s_camera_sd_status.last_error_code = CAMERA_SD_OK;
        return CAMERA_SD_OK;
    }

    ++s_camera_sd_status.card_info_read_error_count;
    s_camera_sd_status.last_error_code = CAMERA_SD_ERR_CARD_INFO_FAILED;
    return CAMERA_SD_ERR_CARD_INFO_FAILED;
}

static uint32_t Camera_SDStorage_WaitCardTransfer(uint32_t timeout_ms)
{
    uint32_t start_ms;
    uint32_t elapsed_ms;
    HAL_SD_CardStateTypeDef card_state;

    if ((s_camera_sd_status.is_initialized != 1U) ||
        (s_camera_sd_status.sdio_ready != 1U))
    {
        s_camera_sd_status.last_error_code =
            CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
        return CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
    }

    ++s_camera_sd_status.block_read_wait_transfer_attempt_count;
    s_camera_sd_status.last_block_read_wait_timeout_ms = timeout_ms;
    start_ms = HAL_GetTick();

    for (;;)
    {
        card_state = HAL_SD_GetCardState(&hsd_snapshot);
        s_camera_sd_status.last_block_read_wait_card_state =
            (uint32_t)card_state;
        elapsed_ms = HAL_GetTick() - start_ms;

        if (card_state == HAL_SD_CARD_TRANSFER)
        {
            ++s_camera_sd_status.block_read_wait_transfer_success_count;
            s_camera_sd_status.last_block_read_wait_operation_ms = elapsed_ms;
            return CAMERA_SD_OK;
        }

        if (elapsed_ms >= timeout_ms)
        {
            ++s_camera_sd_status.block_read_wait_transfer_error_count;
            s_camera_sd_status.last_block_read_wait_operation_ms = elapsed_ms;
            s_camera_sd_status.last_error_code =
                CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
            return CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
        }

        HAL_Delay(1U);
    }
}

/* 与 C5R 读前等待使用相同的 polling 方式，但只更新独立调试状态。 */
static uint32_t Camera_SDStorage_DebugWaitCardTransfer(uint32_t timeout_ms)
{
    uint32_t start_ms = HAL_GetTick();
    uint32_t elapsed_ms;
    HAL_SD_CardStateTypeDef card_state = HAL_SD_GetCardState(&hsd_snapshot);

    s_bus_width_debug.last_wait_timeout_ms = timeout_ms;
    s_bus_width_debug.last_wait_operation_ms = 0U;
    s_bus_width_debug.last_pre_card_state = (uint32_t)card_state;

    for (;;)
    {
        s_bus_width_debug.last_wait_card_state = (uint32_t)card_state;
        elapsed_ms = HAL_GetTick() - start_ms;

        if (card_state == HAL_SD_CARD_TRANSFER)
        {
            s_bus_width_debug.last_wait_operation_ms = elapsed_ms;
            return CAMERA_SD_OK;
        }

        if (elapsed_ms >= timeout_ms)
        {
            s_bus_width_debug.last_wait_operation_ms = elapsed_ms;
            return CAMERA_SD_ERR_BUS_WIDTH_WAIT_TRANSFER_FAILED;
        }

        HAL_Delay(1U);
        card_state = HAL_SD_GetCardState(&hsd_snapshot);
    }
}

static uint32_t Camera_SDStorage_ReadBlockTest(uint32_t block_addr)
{
    uint8_t *read_buffer = (uint8_t *)s_sd_read_test_words;
    uint32_t start_ms;
    uint32_t index;
    uint32_t sum = 0U;
    uint32_t xor_value = 0U;
    uint32_t nonzero_count = 0U;
    uint32_t wait_result;
    HAL_StatusTypeDef hal_status;

    if ((s_camera_sd_status.block_read_test_enabled != 1U) ||
        (s_camera_sd_status.is_initialized != 1U) ||
        (s_camera_sd_status.sdio_ready != 1U) ||
        (s_camera_sd_status.card_info_read_success_count == 0U) ||
        ((s_camera_sd_status.card_log_block_size != 512U) &&
         (s_camera_sd_status.card_block_size != 512U)) ||
        ((s_camera_sd_status.card_log_block_nbr != 0U) &&
         (block_addr >= s_camera_sd_status.card_log_block_nbr)))
    {
        /* 前置条件不满足时不访问硬件，也不覆盖上一次成功读取的缓存。 */
        s_camera_sd_status.last_error_code =
            CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
        return CAMERA_SD_ERR_BLOCK_READ_NOT_READY;
    }

    Camera_SDStorage_CaptureSdioRegSnapshot(&s_read_reg_diag.before_wait);
    wait_result = Camera_SDStorage_WaitCardTransfer(1000U);
    if (wait_result != CAMERA_SD_OK)
    {
        return wait_result;
    }

    Camera_SDStorage_CaptureSdioRegSnapshot(&s_read_reg_diag.before_read);
    s_read_reg_diag.last_hal_state_before_read =
        (uint32_t)hsd_snapshot.State;
    s_camera_sd_status.last_block_read_pre_card_state =
        (uint32_t)HAL_SD_GetCardState(&hsd_snapshot);
    memset(read_buffer, 0, sizeof(s_sd_read_test_words));
    ++s_camera_sd_status.block_read_attempt_count;
    s_camera_sd_status.last_block_read_addr = block_addr;
    s_camera_sd_status.last_block_read_count = 1U;

    start_ms = HAL_GetTick();
    hal_status = HAL_SD_ReadBlocks(
        &hsd_snapshot,
        read_buffer,
        block_addr,
        1U,
        1000U);
    Camera_SDStorage_CaptureSdioRegSnapshot(&s_read_reg_diag.after_read);
    s_read_reg_diag.last_hal_state_after_read =
        (uint32_t)hsd_snapshot.State;
    s_read_reg_diag.last_hal_error_after_read =
        HAL_SD_GetError(&hsd_snapshot);
    s_camera_sd_status.last_block_read_operation_ms =
        HAL_GetTick() - start_ms;
    s_camera_sd_status.last_block_read_status = (uint32_t)hal_status;
    s_camera_sd_status.last_block_read_error =
        s_read_reg_diag.last_hal_error_after_read;
    s_camera_sd_status.last_block_read_error_is_data_crc_fail =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_DATA_CRC_FAIL) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_block_read_error_is_cmd_crc_fail =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_CMD_CRC_FAIL) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_block_read_error_is_cmd_rsp_timeout =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_CMD_RSP_TIMEOUT) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_block_read_error_is_data_timeout =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_DATA_TIMEOUT) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_block_read_error_is_rx_overrun =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_RX_OVERRUN) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_block_read_error_is_tx_underrun =
        ((s_camera_sd_status.last_block_read_error &
          HAL_SD_ERROR_TX_UNDERRUN) != 0U) ? 1U : 0U;
    s_camera_sd_status.last_hal_sd_state =
        (uint32_t)HAL_SD_GetState(&hsd_snapshot);
    s_camera_sd_status.last_block_read_post_card_state =
        (uint32_t)HAL_SD_GetCardState(&hsd_snapshot);
    s_camera_sd_status.last_hal_sd_card_state =
        s_camera_sd_status.last_block_read_post_card_state;

    if (hal_status != HAL_OK)
    {
        ++s_camera_sd_status.block_read_error_count;
        s_camera_sd_status.last_block_read_size = 0U;
        s_camera_sd_status.last_block_read_sum = 0U;
        s_camera_sd_status.last_block_read_xor = 0U;
        s_camera_sd_status.last_block_read_nonzero_count = 0U;
        memset(
            s_camera_sd_status.last_block_read_first16,
            0,
            sizeof(s_camera_sd_status.last_block_read_first16));
        s_camera_sd_status.last_error_code =
            CAMERA_SD_ERR_BLOCK_READ_FAILED;
        return CAMERA_SD_ERR_BLOCK_READ_FAILED;
    }

    for (index = 0U; index < sizeof(s_sd_read_test_words); ++index)
    {
        uint32_t value = read_buffer[index];

        sum += value;
        xor_value ^= value;
        if (value != 0U)
        {
            ++nonzero_count;
        }
    }

    ++s_camera_sd_status.block_read_success_count;
    s_camera_sd_status.last_block_read_size =
        (uint32_t)sizeof(s_sd_read_test_words);
    s_camera_sd_status.last_block_read_sum = sum;
    s_camera_sd_status.last_block_read_xor = xor_value;
    s_camera_sd_status.last_block_read_nonzero_count = nonzero_count;
    memcpy(
        s_camera_sd_status.last_block_read_first16,
        read_buffer,
        sizeof(s_camera_sd_status.last_block_read_first16));
    s_camera_sd_status.last_error_code = CAMERA_SD_OK;
    return CAMERA_SD_OK;
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
    s_camera_sd_status.card_info_read_attempt_count = 0U;
    s_camera_sd_status.card_info_read_success_count = 0U;
    s_camera_sd_status.card_info_read_error_count = 0U;
    s_camera_sd_status.last_card_info_status = (uint32_t)HAL_OK;
    s_camera_sd_status.last_card_info_error = HAL_SD_ERROR_NONE;
    s_camera_sd_status.last_card_info_operation_ms = 0U;
    s_camera_sd_status.last_hal_sd_state = 0U;
    s_camera_sd_status.last_hal_sd_card_state = 0U;
    s_camera_sd_status.card_type = 0U;
    s_camera_sd_status.card_version = 0U;
    s_camera_sd_status.card_class = 0U;
    s_camera_sd_status.card_rel_card_add = 0U;
    s_camera_sd_status.card_block_nbr = 0U;
    s_camera_sd_status.card_block_size = 0U;
    s_camera_sd_status.card_log_block_nbr = 0U;
    s_camera_sd_status.card_log_block_size = 0U;
    s_camera_sd_status.block_read_test_enabled = 1U;
    s_camera_sd_status.block_read_attempt_count = 0U;
    s_camera_sd_status.block_read_success_count = 0U;
    s_camera_sd_status.block_read_error_count = 0U;
    s_camera_sd_status.last_block_read_status = (uint32_t)HAL_OK;
    s_camera_sd_status.last_block_read_error = HAL_SD_ERROR_NONE;
    s_camera_sd_status.last_block_read_operation_ms = 0U;
    s_camera_sd_status.last_block_read_addr = 0U;
    s_camera_sd_status.last_block_read_count = 0U;
    s_camera_sd_status.last_block_read_size = 0U;
    s_camera_sd_status.last_block_read_sum = 0U;
    s_camera_sd_status.last_block_read_xor = 0U;
    s_camera_sd_status.last_block_read_nonzero_count = 0U;
    memset(
        s_camera_sd_status.last_block_read_first16,
        0,
        sizeof(s_camera_sd_status.last_block_read_first16));
    s_camera_sd_status.block_read_wait_transfer_attempt_count = 0U;
    s_camera_sd_status.block_read_wait_transfer_success_count = 0U;
    s_camera_sd_status.block_read_wait_transfer_error_count = 0U;
    s_camera_sd_status.last_block_read_pre_card_state = 0U;
    s_camera_sd_status.last_block_read_post_card_state = 0U;
    s_camera_sd_status.last_block_read_wait_card_state = 0U;
    s_camera_sd_status.last_block_read_wait_operation_ms = 0U;
    s_camera_sd_status.last_block_read_wait_timeout_ms = 0U;
    s_camera_sd_status.last_block_read_error_is_data_crc_fail = 0U;
    s_camera_sd_status.last_block_read_error_is_cmd_crc_fail = 0U;
    s_camera_sd_status.last_block_read_error_is_cmd_rsp_timeout = 0U;
    s_camera_sd_status.last_block_read_error_is_data_timeout = 0U;
    s_camera_sd_status.last_block_read_error_is_rx_overrun = 0U;
    s_camera_sd_status.last_block_read_error_is_tx_underrun = 0U;
    memset(&s_read_reg_diag, 0, sizeof(s_read_reg_diag));
}

void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status)
{
    if (status == NULL)
    {
        return;
    }

    *status = s_camera_sd_status;
}

uint32_t Camera_SDStorage_DebugSetBusWidth(uint32_t bus_width)
{
    uint32_t wait_result;
    uint32_t hal_bus_width;
    uint32_t start_ms;
    HAL_StatusTypeDef hal_status;

    if ((bus_width != 1U) && (bus_width != 4U))
    {
        ++s_bus_width_debug.error_count;
        s_bus_width_debug.last_result = CAMERA_SD_ERR_BUS_WIDTH_INVALID;
        return CAMERA_SD_ERR_BUS_WIDTH_INVALID;
    }

    if ((s_camera_sd_status.is_initialized != 1U) ||
        (s_camera_sd_status.sdio_ready != 1U))
    {
        ++s_bus_width_debug.error_count;
        s_bus_width_debug.last_result = CAMERA_SD_ERR_BUS_WIDTH_NOT_READY;
        return CAMERA_SD_ERR_BUS_WIDTH_NOT_READY;
    }

    s_bus_width_debug.requested_width = bus_width;
    ++s_bus_width_debug.attempt_count;

    wait_result = Camera_SDStorage_DebugWaitCardTransfer(1000U);
    if (wait_result != CAMERA_SD_OK)
    {
        ++s_bus_width_debug.error_count;
        s_bus_width_debug.last_result =
            CAMERA_SD_ERR_BUS_WIDTH_WAIT_TRANSFER_FAILED;
        return CAMERA_SD_ERR_BUS_WIDTH_WAIT_TRANSFER_FAILED;
    }

    s_bus_width_debug.last_pre_card_state =
        (uint32_t)HAL_SD_GetCardState(&hsd_snapshot);
    hal_bus_width = (bus_width == 1U)
        ? SDIO_BUS_WIDE_1B
        : SDIO_BUS_WIDE_4B;
    start_ms = HAL_GetTick();
    hal_status = HAL_SD_ConfigWideBusOperation(
        &hsd_snapshot,
        hal_bus_width);
    s_bus_width_debug.last_operation_ms = HAL_GetTick() - start_ms;
    s_bus_width_debug.last_hal_status = (uint32_t)hal_status;
    s_bus_width_debug.last_hal_error = HAL_SD_GetError(&hsd_snapshot);
    s_bus_width_debug.last_post_card_state =
        (uint32_t)HAL_SD_GetCardState(&hsd_snapshot);

    if (hal_status == HAL_OK)
    {
        s_bus_width_debug.active_width = bus_width;
        ++s_bus_width_debug.success_count;
        s_bus_width_debug.last_result = CAMERA_SD_OK;
        return CAMERA_SD_OK;
    }

    ++s_bus_width_debug.error_count;
    s_bus_width_debug.last_result = CAMERA_SD_ERR_BUS_WIDTH_CONFIG_FAILED;
    return CAMERA_SD_ERR_BUS_WIDTH_CONFIG_FAILED;
}

void Camera_SDStorage_DebugPrintBusWidthStatus(void)
{
    /* LOG_RAW 使用项目现有 printf 风格串口输出，不依赖 CLI 私有函数。 */
    LOG_RAW("SD BUSWIDTH:\r\n");
    LOG_RAW("  supported=%lu\r\n", (unsigned long)s_bus_width_debug.supported);
    LOG_RAW("  requested_width=%lu\r\n", (unsigned long)s_bus_width_debug.requested_width);
    LOG_RAW("  active_width=%lu\r\n", (unsigned long)s_bus_width_debug.active_width);
    LOG_RAW("  attempt_count=%lu\r\n", (unsigned long)s_bus_width_debug.attempt_count);
    LOG_RAW("  success_count=%lu\r\n", (unsigned long)s_bus_width_debug.success_count);
    LOG_RAW("  error_count=%lu\r\n", (unsigned long)s_bus_width_debug.error_count);
    LOG_RAW("  last_result=%lu\r\n", (unsigned long)s_bus_width_debug.last_result);
    LOG_RAW("  last_hal_status=%lu\r\n", (unsigned long)s_bus_width_debug.last_hal_status);
    LOG_RAW("  last_hal_error=0x%08lX\r\n", (unsigned long)s_bus_width_debug.last_hal_error);
    LOG_RAW("  last_pre_card_state=%lu\r\n", (unsigned long)s_bus_width_debug.last_pre_card_state);
    LOG_RAW("  last_post_card_state=%lu\r\n", (unsigned long)s_bus_width_debug.last_post_card_state);
    LOG_RAW("  last_wait_card_state=%lu\r\n", (unsigned long)s_bus_width_debug.last_wait_card_state);
    LOG_RAW("  last_operation_ms=%lu\r\n", (unsigned long)s_bus_width_debug.last_operation_ms);
    LOG_RAW("  last_wait_operation_ms=%lu\r\n", (unsigned long)s_bus_width_debug.last_wait_operation_ms);
    LOG_RAW("  last_wait_timeout_ms=%lu\r\n", (unsigned long)s_bus_width_debug.last_wait_timeout_ms);
}

void Camera_SDStorage_DebugPrintReadRegDiag(void)
{
    const Camera_SDSdioRegSnapshot_t *before_wait =
        &s_read_reg_diag.before_wait;
    const Camera_SDSdioRegSnapshot_t *before_read =
        &s_read_reg_diag.before_read;
    const Camera_SDSdioRegSnapshot_t *after_read =
        &s_read_reg_diag.after_read;
    uint32_t after_sta = after_read->sta;

    LOG_RAW("  read_before_wait_sta=0x%08lX\r\n", (unsigned long)before_wait->sta);
    LOG_RAW("  read_before_wait_clkcr=0x%08lX\r\n", (unsigned long)before_wait->clkcr);
    LOG_RAW("  read_before_wait_dctrl=0x%08lX\r\n", (unsigned long)before_wait->dctrl);
    LOG_RAW("  read_before_wait_dlen=%lu\r\n", (unsigned long)before_wait->dlen);
    LOG_RAW("  read_before_wait_dcount=%lu\r\n", (unsigned long)before_wait->dcount);
    LOG_RAW("  read_before_wait_fifocnt=%lu\r\n", (unsigned long)before_wait->fifocnt);
    LOG_RAW("  read_before_wait_power=0x%08lX\r\n", (unsigned long)before_wait->power);
    LOG_RAW("  read_before_wait_arg=0x%08lX\r\n", (unsigned long)before_wait->arg);
    LOG_RAW("  read_before_wait_cmd=0x%08lX\r\n", (unsigned long)before_wait->cmd);
    LOG_RAW("  read_before_wait_resp_cmd=0x%08lX\r\n", (unsigned long)before_wait->resp_cmd);

    LOG_RAW("  read_before_read_sta=0x%08lX\r\n", (unsigned long)before_read->sta);
    LOG_RAW("  read_before_read_clkcr=0x%08lX\r\n", (unsigned long)before_read->clkcr);
    LOG_RAW("  read_before_read_dctrl=0x%08lX\r\n", (unsigned long)before_read->dctrl);
    LOG_RAW("  read_before_read_dlen=%lu\r\n", (unsigned long)before_read->dlen);
    LOG_RAW("  read_before_read_dcount=%lu\r\n", (unsigned long)before_read->dcount);
    LOG_RAW("  read_before_read_fifocnt=%lu\r\n", (unsigned long)before_read->fifocnt);
    LOG_RAW("  read_before_read_power=0x%08lX\r\n", (unsigned long)before_read->power);
    LOG_RAW("  read_before_read_arg=0x%08lX\r\n", (unsigned long)before_read->arg);
    LOG_RAW("  read_before_read_cmd=0x%08lX\r\n", (unsigned long)before_read->cmd);
    LOG_RAW("  read_before_read_resp_cmd=0x%08lX\r\n", (unsigned long)before_read->resp_cmd);

    LOG_RAW("  read_after_read_sta=0x%08lX\r\n", (unsigned long)after_read->sta);
    LOG_RAW("  read_after_read_clkcr=0x%08lX\r\n", (unsigned long)after_read->clkcr);
    LOG_RAW("  read_after_read_dctrl=0x%08lX\r\n", (unsigned long)after_read->dctrl);
    LOG_RAW("  read_after_read_dlen=%lu\r\n", (unsigned long)after_read->dlen);
    LOG_RAW("  read_after_read_dcount=%lu\r\n", (unsigned long)after_read->dcount);
    LOG_RAW("  read_after_read_fifocnt=%lu\r\n", (unsigned long)after_read->fifocnt);
    LOG_RAW("  read_after_read_power=0x%08lX\r\n", (unsigned long)after_read->power);
    LOG_RAW("  read_after_read_arg=0x%08lX\r\n", (unsigned long)after_read->arg);
    LOG_RAW("  read_after_read_cmd=0x%08lX\r\n", (unsigned long)after_read->cmd);
    LOG_RAW("  read_after_read_resp_cmd=0x%08lX\r\n", (unsigned long)after_read->resp_cmd);
    LOG_RAW("  read_after_read_resp1=0x%08lX\r\n", (unsigned long)after_read->resp1);
    LOG_RAW("  read_after_read_resp2=0x%08lX\r\n", (unsigned long)after_read->resp2);
    LOG_RAW("  read_after_read_resp3=0x%08lX\r\n", (unsigned long)after_read->resp3);
    LOG_RAW("  read_after_read_resp4=0x%08lX\r\n", (unsigned long)after_read->resp4);
    LOG_RAW("  read_hal_state_before_read=%lu\r\n", (unsigned long)s_read_reg_diag.last_hal_state_before_read);
    LOG_RAW("  read_hal_state_after_read=%lu\r\n", (unsigned long)s_read_reg_diag.last_hal_state_after_read);
    LOG_RAW("  read_hal_error_after_read=0x%08lX\r\n", (unsigned long)s_read_reg_diag.last_hal_error_after_read);
    LOG_RAW("  read_after_sta_dcrc_fail=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_DCRCFAIL));
    LOG_RAW("  read_after_sta_dtimeout=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_DTIMEOUT));
    LOG_RAW("  read_after_sta_rxoverr=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_RXOVERR));
    LOG_RAW("  read_after_sta_stbiterr=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_STBITERR));
    LOG_RAW("  read_after_sta_dataend=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_DATAEND));
    LOG_RAW("  read_after_sta_dbckend=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_DBCKEND));
    LOG_RAW("  read_after_sta_cmdsent=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_CMDSENT));
    LOG_RAW("  read_after_sta_cmdrend=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_CMDREND));
    LOG_RAW("  read_after_sta_rxact=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_RXACT));
    LOG_RAW("  read_after_sta_rxdavl=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_RXDAVL));
    LOG_RAW("  read_after_sta_txunderr=%lu\r\n", (unsigned long)Camera_SDStorage_IsStaFlagSet(after_sta, SDIO_STA_TXUNDERR));
}

void Camera_SDStorage_PrintLineState(void)
{
    uint32_t index;

    LOG_RAW("SD LINESTATE:\r\n");
    LOG_RAW("  linestate_readonly=1\r\n");
    LOG_RAW("  linestate_hal_sd_api_call=0\r\n");
    LOG_RAW("  is_initialized=%lu\r\n", (unsigned long)s_camera_sd_status.is_initialized);
    LOG_RAW("  sdio_ready=%lu\r\n", (unsigned long)s_camera_sd_status.sdio_ready);
    LOG_RAW("  sdio_full_gpio_af12_selected=%lu\r\n", (unsigned long)s_camera_sd_status.sdio_full_gpio_af12_selected);
    LOG_RAW("  sdio_af12_selected=%lu\r\n", (unsigned long)s_camera_sd_status.sdio_af12_selected);
    LOG_RAW("  conflict_pins_released=%lu\r\n", (unsigned long)s_camera_sd_status.conflict_pins_released);
    LOG_RAW("  hal_sd_state=%lu\r\n", (unsigned long)s_camera_sd_status.last_hal_sd_state);
    LOG_RAW("  hal_sd_card_state=%lu\r\n", (unsigned long)s_camera_sd_status.last_hal_sd_card_state);
    LOG_RAW("  gpioc_moder=0x%08lX\r\n", (unsigned long)GPIOC->MODER);
    LOG_RAW("  gpioc_pupdr=0x%08lX\r\n", (unsigned long)GPIOC->PUPDR);
    LOG_RAW("  gpioc_ospeedr=0x%08lX\r\n", (unsigned long)GPIOC->OSPEEDR);
    LOG_RAW("  gpioc_afr1=0x%08lX\r\n", (unsigned long)GPIOC->AFR[1]);
    LOG_RAW("  gpioc_idr=0x%08lX\r\n", (unsigned long)GPIOC->IDR);
    LOG_RAW("  gpiod_moder=0x%08lX\r\n", (unsigned long)GPIOD->MODER);
    LOG_RAW("  gpiod_pupdr=0x%08lX\r\n", (unsigned long)GPIOD->PUPDR);
    LOG_RAW("  gpiod_ospeedr=0x%08lX\r\n", (unsigned long)GPIOD->OSPEEDR);
    LOG_RAW("  gpiod_afr0=0x%08lX\r\n", (unsigned long)GPIOD->AFR[0]);
    LOG_RAW("  gpiod_idr=0x%08lX\r\n", (unsigned long)GPIOD->IDR);

    for (index = 0U;
         index < (sizeof(s_camera_sd_lines) / sizeof(s_camera_sd_lines[0]));
         ++index)
    {
        const Camera_SDLineDef_t *line = &s_camera_sd_lines[index];

        LOG_RAW(
            "  %s_mode=%lu\r\n",
            line->name,
            (unsigned long)Camera_SDStorage_GetPinMode(
                line->port,
                line->pin_number));
        LOG_RAW(
            "  %s_pull=%lu\r\n",
            line->name,
            (unsigned long)Camera_SDStorage_GetPinPull(
                line->port,
                line->pin_number));
        LOG_RAW(
            "  %s_speed=%lu\r\n",
            line->name,
            (unsigned long)Camera_SDStorage_GetPinSpeed(
                line->port,
                line->pin_number));
        LOG_RAW(
            "  %s_af=%lu\r\n",
            line->name,
            (unsigned long)Camera_SDStorage_GetPinAf(
                line->port,
                line->pin_number));
        LOG_RAW(
            "  %s_idr=%lu\r\n",
            line->name,
            (unsigned long)Camera_SDStorage_GetPinIdr(
                line->port,
                line->pin_number));
    }
}

uint32_t Camera_SDStorage_RequestBlockReadTest(uint32_t block_addr)
{
    return Camera_SDStorage_ReadBlockTest(block_addr);
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
        uint32_t card_info_result;

        ++s_camera_sd_status.init_success_count;
        ++s_camera_sd_status.sdio_hal_init_success_count;
        s_camera_sd_status.is_initialized = 1U;
        s_camera_sd_status.sdio_ready = 1U;
        card_info_result = Camera_SDStorage_ReadCardInfo();
        s_camera_sd_status.last_operation_ms = HAL_GetTick() - start_ms;
        return card_info_result;
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

        case CAMERA_SD_ERR_CARD_INFO_FAILED:
            return "CARD_INFO_FAILED";

        case CAMERA_SD_ERR_BLOCK_READ_NOT_READY:
            return "BLOCK_READ_NOT_READY";

        case CAMERA_SD_ERR_BLOCK_READ_FAILED:
            return "BLOCK_READ_FAILED";

        case CAMERA_SD_ERR_BUS_WIDTH_NOT_READY:
            return "BUS_WIDTH_NOT_READY";

        case CAMERA_SD_ERR_BUS_WIDTH_INVALID:
            return "BUS_WIDTH_INVALID";

        case CAMERA_SD_ERR_BUS_WIDTH_WAIT_TRANSFER_FAILED:
            return "BUS_WIDTH_WAIT_TRANSFER_FAILED";

        case CAMERA_SD_ERR_BUS_WIDTH_CONFIG_FAILED:
            return "BUS_WIDTH_CONFIG_FAILED";

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
