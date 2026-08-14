#include "camera_sd_storage.h"       // SD takeover、FatFs、BMP 和状态缓存公开接口

#include "bsp_sccb.h"                // 保存、屏蔽和恢复 OV5640 0x3018 DVP pad
#include "bsp_log.h"                 // 仅在 SD/FatFs 失败现场输出最小诊断
#include "camera_dcmi_dma.h"         // 停止/恢复 DCMI 及 DMA 句柄关联
#include "camera_frame_buffer.h"     // 获取已提交的稳定 front frame
#include "camera_rtos.h"             // 复用 DUMP 的公共 RGB565 帧准备路径
#include "camera_snapshot_control.h" // takeover guard、暂停状态和请求阻止统计
#include "ff.h"                      // FatFs mount、stat、open、write、close API
#include "diskio.h"                  // FatFs ioctl 命令和块设备返回类型
#include "stm32f4xx_hal.h"           // SDIO、GPIO、时钟和 HAL SD polling API
#include "FreeRTOS.h"                // task.h 所需内核基础定义
#include "queue.h"                   // Storage request/result depth-one queues
#include "task.h"                    // 写块 polling 期间仅暂停 Task 调度

#include <stddef.h>                   // 提供 NULL
#include <string.h>                   // staging 复制、BMP header 清零和文件名缓存复制

//============================================================================
// @file    camera_sd_storage.c
// @brief   摄像头共享引脚接管、FatFs 挂载和 BMP24 快照保存模块
//
// 完整链路：公共 prepare frame → stable front → 独立 staging → 暂停 camera/guard →
// 保存并屏蔽 OV5640 0x3018[6:4] → GPIO 从 DCMI AF13 切到 SDIO AF12 →
// SDIO 1-bit polling → FatFs → 逐行 RGB565 转 BMP24 → 统一 cleanup/restore。
//
// 必须先准备并复制图像，因为 PC8/PC9/PC11 接管给 SDIO 后摄像头不能继续采集；
// 仅取得 front 指针也不等于长期锁住双缓冲，staging 才能保证写卡期间数据稳定。
// 本模块保持硬件验证过的 1-bit polling 策略，不启用 SDIO DMA/IRQ。
// SD STATUS 只读 s_camera_sd_status 缓存；真实探测、mount 或 takeover 只能由显式命令触发。
// 所有失败都进入统一清理，先完成仍依赖 SD 的文件/文件系统操作，再恢复共享硬件。
//============================================================================

// OV5640 DVP pad output enable 02；项目已验证 bit[4]/[5]/[6] 控制 D2/D3/D4。
#define CAMERA_SD_DVP_PAD_OUTPUT_ENABLE02_REG 0x3018U
// 0x8F=1000 1111b：保留其他位并清零 bit[6:4]，临时关闭 D2/D3/D4 输出。
// D2/D3/D4 分别连接 PC8/PC9/PC11，并与 SDIO D0/D1/D3 复用。
#define CAMERA_SD_DVP_CONFLICT_PAD_KEEP_MASK  0x8FU
#define CAMERA_SD_REG_VALUE_UNKNOWN           0xFFFFFFFFU
#define CAMERA_SD_FATFS_CARD_TIMEOUT_MS       1000U
#define CAMERA_SD_FATFS_READ_TIMEOUT_MS       1000U
#define CAMERA_SD_FATFS_WRITE_TIMEOUT_MS      1000U
#define CAMERA_SD_FATFS_MOUNT_NOT_RUN         0xFFFFFFFFU
#define CAMERA_SD_RESTORE_LCD_WIDTH            480U
#define CAMERA_SD_RESTORE_LCD_HEIGHT           320U
#define CAMERA_SD_CAMERA_RESTORE_DELAY_MS       100U
#define CAMERA_SD_SNAPSHOT_FILE_NAME_SIZE           12U
#define CAMERA_SD_SNAPSHOT_FILE_INDEX_MIN             1U
#define CAMERA_SD_SNAPSHOT_FILE_INDEX_MAX          9999U
#define CAMERA_SD_NO_FILE_NAME_TEXT               "NONE"
#define CAMERA_SD_SNAPSHOT_FORMAT_TEXT         "BMP24"
#define CAMERA_SD_SNAPSHOT_SOURCE_TEXT         "FRONT"
#define CAMERA_SD_FRAME_PREPARE_MAX_RETRIES      3U
#define CAMERA_SD_FRAME_PREPARE_RETRY_DELAY_MS  75U
#define CAMERA_SD_BMP_FILE_HEADER_SIZE           14U
#define CAMERA_SD_BMP_INFO_HEADER_SIZE           40U
// BMP 文件头 14 字节加 BITMAPINFOHEADER 40 字节，像素数据从偏移 54 开始。
#define CAMERA_SD_BMP_HEADER_SIZE \
    (CAMERA_SD_BMP_FILE_HEADER_SIZE + CAMERA_SD_BMP_INFO_HEADER_SIZE)
#define CAMERA_SD_BMP_BYTES_PER_PIXEL             3U
#define CAMERA_SD_BMP_ROW_BYTES \
    (CAMERA_FB_WIDTH * CAMERA_SD_BMP_BYTES_PER_PIXEL)
// BMP 行必须按 4 字节对齐；160x3=480 已自然对齐，所以 stride 仍为 480。
#define CAMERA_SD_BMP_ROW_STRIDE \
    ((CAMERA_SD_BMP_ROW_BYTES + 3U) & ~3U)
#define CAMERA_SD_BMP_IMAGE_SIZE \
    (CAMERA_SD_BMP_ROW_STRIDE * CAMERA_FB_HEIGHT)
#define CAMERA_SD_BMP_FILE_SIZE \
    (CAMERA_SD_BMP_HEADER_SIZE + CAMERA_SD_BMP_IMAGE_SIZE)

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

typedef struct
{
    uint32_t request_id;
    uint32_t total_start_tick;
    uint32_t restore_continuous_capture;
} CameraStorageRequest_t;

typedef struct
{
    uint32_t request_id;
    uint32_t result;
} CameraStorageResponse_t;

static const CameraSdLine_t s_camera_sd_lines[] =
{
    {GPIOC, 8U},
    {GPIOC, 9U},
    {GPIOC, 10U},
    {GPIOC, 11U},
    {GPIOC, 12U},
    {GPIOD, 2U}
};

// 表中依次为 PC8=D0、PC9=D1、PC10=D2、PC11=D3、PC12=CLK、PD2=CMD；
// 接管后逐线回读模式/上下拉/速度/AF，固定数组使校验循环具有明确上界。

/*
 * 在接管 SDIO 引脚前复制前台帧，使写卡期间的数据不再依赖摄像头双缓冲状态。
 * 38400 字节暂存区放在文件作用域，避免占用任务栈。
 */
static uint8_t s_camera_sd_snapshot_image_buffer[CAMERA_FB_SIZE_BYTES]
    __attribute__((aligned(4)));
static uint8_t s_camera_sd_bmp_header[CAMERA_SD_BMP_HEADER_SIZE]
    __attribute__((aligned(4)));
/* BMP 按行转为 BGR888，复用 480 字节行缓冲，避免额外申请 57600 字节全帧缓冲。 */
static uint8_t s_camera_sd_bmp_row_buffer[CAMERA_SD_BMP_ROW_STRIDE]
    __attribute__((aligned(4)));
static char s_camera_sd_candidate_file_name[
    CAMERA_SD_SNAPSHOT_FILE_NAME_SIZE];
static char s_camera_sd_last_file_name[
    CAMERA_SD_SNAPSHOT_FILE_NAME_SIZE];
// SD STATUS 的纯缓存来源；查询函数只复制它，不触发任何硬件访问。
static CameraSdStorageStatus_t s_camera_sd_status;
static SD_HandleTypeDef s_camera_sd_handle;
static HAL_SD_CardInfoTypeDef s_camera_sd_card_info;
static FATFS s_camera_sd_fatfs;
static FIL s_camera_sd_file;
// 以下阶段标志让“部分初始化失败”也能执行对称 cleanup，而不是只清理成功路径。
static uint32_t s_camera_sd_full_gpio_af12_selected;
static uint32_t s_camera_sd_clock_enabled;
static uint32_t s_camera_sd_hal_init_attempted;
static uint32_t s_camera_sd_card_info_valid;
// mount 会回调 diskio，必须在 f_mount 前激活 session；写允许位阻止只读探测意外写卡。
static uint32_t s_camera_sd_fatfs_session_active;
static uint32_t s_camera_sd_fatfs_write_allowed;
// FatFs 只返回通用 FRESULT，该字段保存更具体的 HAL/diskio 根因供上层诊断。
static uint32_t s_camera_sd_fatfs_disk_error;
static QueueHandle_t s_camera_storage_request_queue;
static QueueHandle_t s_camera_storage_result_queue;
static uint32_t s_camera_storage_request_id;
static volatile uint32_t s_camera_storage_stack_min_free_bytes;
static CameraSdSnapshotResult_t s_camera_storage_result_info;

static uint32_t Camera_SDStorage_SaveStagedSnapshotFrame(
    CameraSdSnapshotResult_t *prepared_result,
    uint32_t total_start_tick,
    uint32_t restore_continuous_capture);

// 更新最近一次 SD 操作的缓存错误信息
static void Camera_SDStorage_SetLastError(uint32_t error_code)
{
    s_camera_sd_status.last_error_code = error_code;
    s_camera_sd_status.last_error_text =
        Camera_SDStorage_ErrorToString(error_code);
}

// 更新最近一次图片保存的缓存错误信息
static void Camera_SDStorage_SetSaveError(uint32_t error_code)
{
    s_camera_sd_status.save_error_code = error_code;
    s_camera_sd_status.save_error_text =
        Camera_SDStorage_ErrorToString(error_code);
}

// 按 IMGxxxx.BMP 规则格式化候选文件名
static void Camera_SDStorage_FormatSnapshotFileName(uint32_t file_index)
{
    s_camera_sd_candidate_file_name[0] = 'I';
    s_camera_sd_candidate_file_name[1] = 'M';
    s_camera_sd_candidate_file_name[2] = 'G';
    s_camera_sd_candidate_file_name[3] =
        (char)('0' + ((file_index / 1000U) % 10U));
    s_camera_sd_candidate_file_name[4] =
        (char)('0' + ((file_index / 100U) % 10U));
    s_camera_sd_candidate_file_name[5] =
        (char)('0' + ((file_index / 10U) % 10U));
    s_camera_sd_candidate_file_name[6] =
        (char)('0' + (file_index % 10U));
    s_camera_sd_candidate_file_name[7] = '.';
    s_camera_sd_candidate_file_name[8] = 'B';
    s_camera_sd_candidate_file_name[9] = 'M';
    s_camera_sd_candidate_file_name[10] = 'P';
    s_camera_sd_candidate_file_name[11] = '\0';
}

// 使用 f_stat 顺序寻找首个不存在的 IMG0001.BMP～IMG9999.BMP。
// 循环最多 9999 次：FR_NO_FILE 成功退出，其他错误立即终止；后续 FA_CREATE_NEW
// 仍会再次防止覆盖已有文件，因此扫描和创建之间的变化不会静默覆盖数据。
static uint32_t Camera_SDStorage_FindNextSnapshotFileName(void)
{
    FILINFO file_info;
    FRESULT fatfs_result;
    uint32_t file_index;

    for (file_index = CAMERA_SD_SNAPSHOT_FILE_INDEX_MIN;
         file_index <= CAMERA_SD_SNAPSHOT_FILE_INDEX_MAX;
         ++file_index)
    {
        Camera_SDStorage_FormatSnapshotFileName(file_index);
        fatfs_result = f_stat(
            s_camera_sd_candidate_file_name,
            &file_info);
        if (fatfs_result == FR_NO_FILE)
        {
            return CAMERA_SD_OK;
        }
        if (fatfs_result != FR_OK)
        {
            LOG_RAW("[SD][FSTAT] fr=%d\r\n", (int)fatfs_result);
            s_camera_sd_candidate_file_name[0] = '\0';
            return CAMERA_SD_ERR_FILE_SCAN_FAILED;
        }
    }

    s_camera_sd_candidate_file_name[0] = '\0';
    return CAMERA_SD_ERR_FILE_INDEX_FULL;
}

// 将 16 位数按 BMP 要求的小端格式写入字节数组
static void Camera_SDStorage_WriteU16LE(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

// 将 32 位数按 BMP 要求的小端格式写入字节数组
static void Camera_SDStorage_WriteU32LE(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

// 构造 54 字节 BMP24 header："BM"、总大小、像素偏移、40 字节信息头、宽高、
// plane=1、24 bpp 和图像大小。高度写成负数表示 top-down，可按源帧行序直接写出。
static void Camera_SDStorage_BuildBmp24Header(void)
{
    (void)memset(s_camera_sd_bmp_header, 0, sizeof(s_camera_sd_bmp_header));
    s_camera_sd_bmp_header[0] = 'B';
    s_camera_sd_bmp_header[1] = 'M';
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[2],
        CAMERA_SD_BMP_FILE_SIZE);
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[10],
        CAMERA_SD_BMP_HEADER_SIZE);
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[14],
        CAMERA_SD_BMP_INFO_HEADER_SIZE);
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[18],
        CAMERA_FB_WIDTH);
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[22],
        (uint32_t)(-(int32_t)CAMERA_FB_HEIGHT));
    Camera_SDStorage_WriteU16LE(&s_camera_sd_bmp_header[26], 1U);
    Camera_SDStorage_WriteU16LE(&s_camera_sd_bmp_header[28], 24U);
    Camera_SDStorage_WriteU32LE(
        &s_camera_sd_bmp_header[34],
        CAMERA_SD_BMP_IMAGE_SIZE);
}

// 将暂存帧的一行 RGB565 转成 BMP BGR888。
// RGB565 bit[15:11]=R5、bit[10:5]=G6、bit[4:0]=B5；高位复制扩展到 8 bit，
// 再按 BMP 的 B、G、R 顺序写入 480 字节行缓冲。循环固定 160 个像素。
static void Camera_SDStorage_ConvertBmp24Row(uint32_t row_index)
{
    const uint8_t *source = &s_camera_sd_snapshot_image_buffer[
        row_index * CAMERA_FB_WIDTH * CAMERA_FB_BYTES_PER_PIXEL];
    uint32_t pixel_index;

    for (pixel_index = 0U; pixel_index < CAMERA_FB_WIDTH; ++pixel_index)
    {
        uint16_t pixel = (uint16_t)source[pixel_index * 2U] |
            ((uint16_t)source[(pixel_index * 2U) + 1U] << 8U);
        uint8_t red5 = (uint8_t)((pixel >> 11U) & 0x1FU);
        uint8_t green6 = (uint8_t)((pixel >> 5U) & 0x3FU);
        uint8_t blue5 = (uint8_t)(pixel & 0x1FU);
        uint32_t destination_index = pixel_index * 3U;

        s_camera_sd_bmp_row_buffer[destination_index] =
            (uint8_t)((blue5 << 3U) | (blue5 >> 2U));
        s_camera_sd_bmp_row_buffer[destination_index + 1U] =
            (uint8_t)((green6 << 2U) | (green6 >> 4U));
        s_camera_sd_bmp_row_buffer[destination_index + 2U] =
            (uint8_t)((red5 << 3U) | (red5 >> 2U));
    }
}

// 先写 54 字节 header，再固定循环 120 行，每行转换并写 480 字节。
// 这样只复用 480 字节 row buffer，而不是额外占用 57600 字节 BGR888 全帧 RAM；
// 每次同时检查 FRESULT 和实际写入长度，任一不符都进入主流程 cleanup。
static uint32_t Camera_SDStorage_WriteBmp24(
    FIL *file,
    uint32_t *file_bytes_written)
{
    FRESULT fatfs_result;
    UINT chunk_bytes_written;
    uint32_t row_index;
    uint32_t total_bytes_written = 0U;

    if ((file == NULL) || (file_bytes_written == NULL))
    {
        return CAMERA_SD_ERR_INVALID_ARGUMENT;
    }

    *file_bytes_written = 0U;
    Camera_SDStorage_BuildBmp24Header();
    fatfs_result = f_write(
        file,
        s_camera_sd_bmp_header,
        (UINT)CAMERA_SD_BMP_HEADER_SIZE,
        &chunk_bytes_written);
    if ((fatfs_result != FR_OK) ||
        (chunk_bytes_written != (UINT)CAMERA_SD_BMP_HEADER_SIZE))
    {
        LOG_RAW("[SD][FWRITE] fr=%d bw=%u\r\n",
                (int)fatfs_result, (unsigned int)chunk_bytes_written);
        return (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED;
    }
    total_bytes_written += (uint32_t)chunk_bytes_written;

    for (row_index = 0U; row_index < CAMERA_FB_HEIGHT; ++row_index)
    {
        Camera_SDStorage_ConvertBmp24Row(row_index);
        fatfs_result = f_write(
            file,
            s_camera_sd_bmp_row_buffer,
            (UINT)CAMERA_SD_BMP_ROW_STRIDE,
            &chunk_bytes_written);
        if ((fatfs_result != FR_OK) ||
            (chunk_bytes_written != (UINT)CAMERA_SD_BMP_ROW_STRIDE))
        {
            LOG_RAW("[SD][FWRITE] fr=%d bw=%u\r\n",
                    (int)fatfs_result, (unsigned int)chunk_bytes_written);
            return (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
                s_camera_sd_fatfs_disk_error :
                CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED;
        }
        total_bytes_written += (uint32_t)chunk_bytes_written;
    }

    if (total_bytes_written != CAMERA_SD_BMP_FILE_SIZE)
    {
        return CAMERA_SD_ERR_FATFS_FILE_WRITE_FAILED;
    }

    *file_bytes_written = total_bytes_written;
    return CAMERA_SD_OK;
}

// 将 FatFs 挂载结果写入只读状态缓存
static void Camera_SDStorage_SetMountResult(FRESULT mount_result)
{
    s_camera_sd_status.last_mount_result = (uint32_t)mount_result;
    s_camera_sd_status.last_mount_text =
        (mount_result == FR_OK) ? "PASS" : "FAIL";
}

// 只保留清理链路遇到的第一个错误，避免后续错误覆盖根因
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

// 确保 SDIO 接管恢复前 DCMI 与原 DMA 句柄仍保持关联
static void Camera_SDStorage_EnsureDcmiDmaHandle(void)
{
    if (g_camera_dcmi.DMA_Handle == NULL)
    {
        __HAL_LINKDMA(&g_camera_dcmi, DMA_Handle, g_camera_dma);
    }
}

// 轮询等待 SD 卡回到 TRANSFER，卡错误/断开或 1000 ms timeout 均退出。
// 每轮 HAL_Delay(1) 避免 CPU 纯忙转；只有 TRANSFER 后才能安全开始下一次块操作。
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

// 读取指定 GPIO 的模式字段
static uint32_t Camera_SDStorage_GetPinMode(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->MODER >> (pin_number * 2U)) & 0x3U;
}

// 读取指定 GPIO 的上下拉字段
static uint32_t Camera_SDStorage_GetPinPull(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->PUPDR >> (pin_number * 2U)) & 0x3U;
}

// 读取指定 GPIO 的速度字段
static uint32_t Camera_SDStorage_GetPinSpeed(
    GPIO_TypeDef *port,
    uint32_t pin_number)
{
    return (port->OSPEEDR >> (pin_number * 2U)) & 0x3U;
}

// 读取指定 GPIO 的复用功能编号
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

// 校验全部 SDIO 引脚是否已切换为 AF12 推挽上拉配置
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

// 先把 DCMI/SDIO 冲突引脚置为输入，避免两端在切换瞬间同时驱动总线
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

// 将 PC8、PC9、PC11 从 DCMI AF13 切换到 SDIO AF12
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

// 配置完整 SDIO 1-bit 所需 GPIO，并提前标记接管以保证失败时也会清理
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

// 退出接管时先把全部 SDIO GPIO 置为高阻输入
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

// 将共享数据线恢复为摄像头 DCMI AF13 配置
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

// 按接管前状态恢复快照保护、DCMI/DMA 关联和连续采集链路
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

// 填充当前硬件验证过的 SDIO 1-bit polling 参数。
// 即使 GPIO 表包含完整 SDIO 线，也不代表启用 4-bit；读写仍调用 HAL_SD_*Blocks polling，
// 不使用 SDIO DMA/IRQ，避免引入额外并发与恢复依赖。
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

// 打开 SDIO 外设时钟并记录状态
static void Camera_SDStorage_EnableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_ENABLE();
    s_camera_sd_clock_enabled = 1U;
}

// 关闭 SDIO 外设时钟并记录状态
static void Camera_SDStorage_DisableSdioClock(void)
{
    __HAL_RCC_SDIO_CLK_DISABLE();
    s_camera_sd_clock_enabled = 0U;
}

// 初始化 SD 存储状态缓存，不访问 SD 卡或共享硬件
void Camera_SDStorage_InitState(void)
{
    memset(&s_camera_sd_status, 0, sizeof(s_camera_sd_status));
    memset(&s_camera_sd_handle, 0, sizeof(s_camera_sd_handle));
    memset(&s_camera_sd_card_info, 0, sizeof(s_camera_sd_card_info));
    memset(&s_camera_sd_fatfs, 0, sizeof(s_camera_sd_fatfs));
    memset(&s_camera_sd_file, 0, sizeof(s_camera_sd_file));
    memset(
        s_camera_sd_candidate_file_name,
        0,
        sizeof(s_camera_sd_candidate_file_name));
    memset(
        s_camera_sd_last_file_name,
        0,
        sizeof(s_camera_sd_last_file_name));
    (void)memcpy(
        s_camera_sd_last_file_name,
        CAMERA_SD_NO_FILE_NAME_TEXT,
        sizeof(CAMERA_SD_NO_FILE_NAME_TEXT));

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
    s_camera_sd_status.last_file_name = s_camera_sd_last_file_name;
    s_camera_sd_status.last_file_size = 0U;
    s_camera_sd_status.save_count = 0U;
    s_camera_sd_status.last_total_ms = 0U;
    s_camera_sd_status.last_write_ms = 0U;
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

bool Camera_SDStorage_TaskInit(void)
{
    s_camera_storage_request_queue = xQueueCreate(
        1U,
        sizeof(CameraStorageRequest_t));
    s_camera_storage_result_queue = xQueueCreate(
        1U,
        sizeof(CameraStorageResponse_t));
    s_camera_storage_request_id = 0U;
    s_camera_storage_stack_min_free_bytes = 0U;

    return ((s_camera_storage_request_queue != NULL) &&
            (s_camera_storage_result_queue != NULL));
}

void Camera_SDStorage_Task(void *argument)
{
    CameraStorageRequest_t request;
    CameraStorageResponse_t response;

    (void)argument;
    s_camera_storage_stack_min_free_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
        (uint32_t)sizeof(StackType_t);
    for (;;)
    {
        (void)xEventGroupSetBits(
            CameraSystemEventGroup,
            CAMERA_SYS_HB_STORAGE);
        if (xQueueReceive(
                s_camera_storage_request_queue,
                &request,
                pdMS_TO_TICKS(CAMERA_RTOS_HEARTBEAT_TIMEOUT_MS)) != pdPASS)
        {
            continue;
        }

        (void)xEventGroupSetBits(
            CameraSystemEventGroup,
            CAMERA_SYS_STORAGE_BUSY | CAMERA_SYS_HB_STORAGE);
        s_camera_storage_stack_min_free_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
            (uint32_t)sizeof(StackType_t);
        response.request_id = request.request_id;
        response.result = Camera_SDStorage_SaveStagedSnapshotFrame(
            &s_camera_storage_result_info,
            request.total_start_tick,
            request.restore_continuous_capture);
        (void)xEventGroupClearBits(
            CameraSystemEventGroup,
            CAMERA_SYS_STORAGE_BUSY);
        (void)xEventGroupSetBits(
            CameraSystemEventGroup,
            CAMERA_SYS_HB_STORAGE);
        s_camera_storage_stack_min_free_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
            (uint32_t)sizeof(StackType_t);
        (void)xQueueSend(s_camera_storage_result_queue, &response, 0U);
    }
}

uint32_t Camera_SDStorage_GetStackMinFreeBytes(void)
{
    return s_camera_storage_stack_min_free_bytes;
}

// 仅读取缓存状态；SD STATUS 通过本接口查询，不能在查询时 mount、takeover 或访问卡。
void Camera_SDStorage_GetStatus(CameraSdStorageStatus_t *status)
{
    if (status != NULL)
    {
        *status = s_camera_sd_status;
    }
}

// 保存 0x3018 后与 0x8F 相与，清零 bit[6:4] 并回读验证。
// 这会关闭共享的 D2/D3/D4 pad，使 OV5640 不再驱动 PC8/PC9/PC11；只有完成这一步，
// STM32 才能安全把 GPIO 从 DCMI AF13 切到 SDIO AF12。
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

// 恢复接管前保存的 OV5640 0x3018 DVP 输出配置
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

// 在摄像头已暂停且 DVP 已屏蔽后，将共享 GPIO 接管给 SDIO
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

// 在 DVP 已屏蔽、GPIO 已 takeover 后按 1-bit polling 初始化 SD 卡并读取容量信息。
// HAL_SD_Init 不能提前调用，否则 OV5640 仍可能驱动共享数据线；任何失败由 exit/cleanup
// 根据 hal_init_attempted、clock_enabled 和 GPIO 阶段标志对称恢复。
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

// 反初始化 SD、关闭时钟并把 SDIO GPIO 退出到安全输入态
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

// 向 FatFs 适配层返回当前磁盘就绪状态
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

// 校验当前接管会话是否已具备 FatFs 磁盘访问条件
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

// 通过 HAL SD polling 接口读取 FatFs 请求的连续扇区
uint32_t Camera_SDStorage_FatFsDiskRead(
    uint8_t *buffer,
    uint32_t sector,
    uint32_t count)
{
    HAL_StatusTypeDef hal_status;
    uint32_t hal_error;
    uint32_t sdio_status;
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
    else if ((sector >= s_camera_sd_card_info.LogBlockNbr) ||
             (count > (s_camera_sd_card_info.LogBlockNbr - sector)))
    {
        result = CAMERA_SD_ERR_INVALID_ARGUMENT;
    }
    else
    {
        result = Camera_SDStorage_WaitForCardTransfer();
        if (result == CAMERA_SD_OK)
        {
            vTaskSuspendAll();
            hal_status = HAL_SD_ReadBlocks(
                &s_camera_sd_handle,
                buffer,
                sector,
                count,
                CAMERA_SD_FATFS_READ_TIMEOUT_MS);
            (void)xTaskResumeAll();
            hal_error = s_camera_sd_handle.ErrorCode;
            sdio_status = SDIO->STA;
            s_camera_sd_status.last_sd_rw_status = (uint32_t)hal_status;
            s_camera_sd_status.last_sd_rw_error = hal_error;
            if (hal_status != HAL_OK)
            {
                LOG_RAW("[SDIO][RFAIL] sector=%lu count=%u hal=%d err=0x%08lX sta=0x%08lX\r\n",
                        (unsigned long)sector, (unsigned int)count, (int)hal_status,
                        (unsigned long)hal_error, (unsigned long)sdio_status);
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

// 仅在显式允许写入的会话中写入 FatFs 请求的连续扇区
uint32_t Camera_SDStorage_FatFsDiskWrite(
    const uint8_t *buffer,
    uint32_t sector,
    uint32_t count)
{
    HAL_StatusTypeDef hal_status;
    uint32_t hal_error;
    uint32_t sdio_status;
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
    else if ((sector >= s_camera_sd_card_info.LogBlockNbr) ||
             (count > (s_camera_sd_card_info.LogBlockNbr - sector)))
    {
        result = CAMERA_SD_ERR_INVALID_ARGUMENT;
    }
    else
    {
        result = Camera_SDStorage_WaitForCardTransfer();
        if (result == CAMERA_SD_OK)
        {
            vTaskSuspendAll();
            hal_status = HAL_SD_WriteBlocks(
                &s_camera_sd_handle,
                (uint8_t *)buffer,
                sector,
                count,
                CAMERA_SD_FATFS_WRITE_TIMEOUT_MS);
            (void)xTaskResumeAll();
            hal_error = s_camera_sd_handle.ErrorCode;
            sdio_status = SDIO->STA;
            s_camera_sd_status.last_sd_rw_status = (uint32_t)hal_status;
            s_camera_sd_status.last_sd_rw_error = hal_error;
            if (hal_status != HAL_OK)
            {
                LOG_RAW("[SDIO][WFAIL] sector=%lu count=%u hal=%d err=0x%08lX sta=0x%08lX\r\n",
                        (unsigned long)sector, (unsigned int)count, (int)hal_status,
                        (unsigned long)hal_error, (unsigned long)sdio_status);
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

// 处理 FatFs 同步和介质参数查询命令
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

// 执行一次挂载检查，并无条件沿统一清理路径恢复摄像头硬件状态
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

    // f_mount(...,1) 会立即识别卷并回调 diskio；必须先激活 session 让 diskio 访问既有卡。
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
    // 即使挂载检查中途失败，也必须先卸载文件系统，再退出 SDIO 接管并恢复 DVP/DCMI。
    if (mount_attempted != 0U)
    {
        // 卸载必须发生在 HAL SD、时钟和 GPIO 仍可访问时，之后才能退出 takeover。
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

// 获取并校验固定 160x120 RGB565 前台帧元数据
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

// 将 front 完整复制到文件作用域 staging，并扫描固定 38400 字节计算非零数/校验和。
// 取得 front 指针并不会锁住双缓冲；独立副本才允许后续暂停摄像头和长时间写卡。
// 扫描循环有固定帧长，无硬件等待，用于拒绝明显全零帧而不是替代 CRC。
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

    // front buffer 仍属于双缓冲采集链路；复制后写卡过程使用独立、稳定的数据快照。
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

// 在摄像头仍可工作时复用 DUMP 的 Camera_RTOS_PrepareRgb565Frame，再立即复制 front。
// 最多 3 次重试即共 4 次尝试，只对“准备成功但 staging 判定空帧”重试；
// RTOS 采集错误或 timeout 直接退出。复制完成后才暂停 camera，可缩短图像链路停机时间。
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
    // retry_count 从 0 到 3，循环具有明确上界；重试间由固定延时隔开。
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

// 执行完整 SD SNAPSHOT 状态机，并让所有成功/失败分支汇合到统一 cleanup。
// prepare/staging 完成后才设置 guard、屏蔽 0x3018 和 takeover，随后 mount、找名、
// FA_CREATE_NEW 打开、逐行写 BMP；任一步失败都保留首个根因并继续恢复其余资源。
uint32_t Camera_SDStorage_SaveSnapshotFrame(
    CameraSdSnapshotResult_t *snapshot_result)
{
    CameraSdSnapshotResult_t result_info;  // ControlTask stack remains valid while it waits for StorageTask
    CameraStorageRequest_t storage_request;
    CameraStorageResponse_t storage_response;
    uint32_t result = CAMERA_SD_OK;        // 保留业务/cleanup 遇到的第一个总错误
    uint32_t step_result;                  // 当前阶段的临时返回码
    uint32_t restore_continuous_capture = 0U; // 记录接管前是否需恢复连续 LCD 采集
    uint32_t total_start_tick = HAL_GetTick();
    uint32_t prepare_start_tick;

    if ((s_camera_storage_request_queue == NULL) ||
        (s_camera_storage_result_queue == NULL))
    {
        return CAMERA_SD_ERR_SNAPSHOT_BUSY;
    }

    memset(&result_info, 0, sizeof(result_info));
    result_info.file_name = CAMERA_SD_NO_FILE_NAME_TEXT;
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
        result_info.total_ms = HAL_GetTick() - total_start_tick;
        s_camera_sd_status.last_total_ms = result_info.total_ms;
        s_camera_sd_status.last_write_ms = result_info.write_ms;
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
    prepare_start_tick = HAL_GetTick();
    // 先准备并复制完整图像，再暂停摄像头和接管共享引脚，缩短图像链路停机时间。
    step_result = Camera_SDStorage_PrepareAndStageFrontFrame(&result_info);
    result_info.prepare_ms = HAL_GetTick() - prepare_start_tick;
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        uint32_t restore_result = Camera_SDStorage_RestoreCameraLink(
            restore_continuous_capture,
            0U);

        Camera_SDStorage_RecordFirstError(&result, restore_result);
        result_info.restore_text =
            (restore_result == CAMERA_SD_OK) ? "PASS" : "FAIL";
        result_info.cleanup_text = result_info.restore_text;
        result_info.total_ms = HAL_GetTick() - total_start_tick;
        result_info.error_code = result;
        result_info.error_text = Camera_SDStorage_ErrorToString(result);
        s_camera_sd_status.last_snapshot_text = "FAIL";
        s_camera_sd_status.last_total_ms = result_info.total_ms;
        Camera_SDStorage_SetSaveError(result);
        Camera_SDStorage_SetLastError(result);
        if (snapshot_result != NULL)
        {
            *snapshot_result = result_info;
        }
        return result;
    }

    storage_request.request_id = ++s_camera_storage_request_id;
    storage_request.total_start_tick = total_start_tick;
    storage_request.restore_continuous_capture = restore_continuous_capture;
    s_camera_storage_result_info = result_info;
    while (xQueueReceive(
               s_camera_storage_result_queue,
               &storage_response,
               0U) == pdPASS)
    {
    }
    if (xQueueSend(
            s_camera_storage_request_queue,
            &storage_request,
            portMAX_DELAY) != pdPASS)
    {
        return CAMERA_SD_ERR_SNAPSHOT_BUSY;
    }
    do
    {
        (void)xQueueReceive(
            s_camera_storage_result_queue,
            &storage_response,
            portMAX_DELAY);
    } while (storage_response.request_id != storage_request.request_id);

    if (snapshot_result != NULL)
    {
        *snapshot_result = s_camera_storage_result_info;
    }
    return storage_response.result;
}

static uint32_t Camera_SDStorage_SaveStagedSnapshotFrame(
    CameraSdSnapshotResult_t *prepared_result,
    uint32_t total_start_tick,
    uint32_t restore_continuous_capture)
{
    CameraSdSnapshotResult_t result_info = *prepared_result;
    uint32_t result = CAMERA_SD_OK;
    uint32_t cleanup_result = CAMERA_SD_OK;
    uint32_t restore_result = CAMERA_SD_OK;
    uint32_t step_result;
    uint32_t camera_restore_required = 1U;
    uint32_t dvp_restore_required = 0U;
    uint32_t takeover_attempted = 0U;
    uint32_t mount_attempted = 0U;
    uint32_t file_opened = 0U;
    uint32_t file_bytes_written = 0U;
    uint32_t write_start_tick;
    uint32_t cleanup_start_tick;
    FRESULT fatfs_result;

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

    // 激活 session 后立即挂载；成功后 f_stat/f_open/f_write 才能使用该卷。
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
    step_result = Camera_SDStorage_FindNextSnapshotFileName();
    if (step_result != CAMERA_SD_OK)
    {
        result = step_result;
        goto cleanup;
    }

    result_info.file_name = s_camera_sd_candidate_file_name;
    s_camera_sd_fatfs_write_allowed = 1U;
    fatfs_result = f_open(
        &s_camera_sd_file,
        s_camera_sd_candidate_file_name,
        FA_CREATE_NEW | FA_WRITE);
    if (fatfs_result != FR_OK)
    {
        result = (s_camera_sd_fatfs_disk_error != CAMERA_SD_OK) ?
            s_camera_sd_fatfs_disk_error :
            CAMERA_SD_ERR_FATFS_FILE_OPEN_FAILED;
        goto cleanup;
    }
    file_opened = 1U;

    write_start_tick = HAL_GetTick();
    step_result = Camera_SDStorage_WriteBmp24(
        &s_camera_sd_file,
        &file_bytes_written);
    result_info.write_ms = HAL_GetTick() - write_start_tick;
    if (step_result != CAMERA_SD_OK)
    {
        result_info.write_text = "FAIL";
        result = step_result;
        goto cleanup;
    }
    result_info.write_text = "PASS";

cleanup:
    /*
     * 清理顺序不能颠倒：
     * 1. f_close 先把 FatFs 缓存刷新到仍可访问的卡；2. 禁止继续写；3. unmount；
     * 4. 结束 FatFs session；5. HAL_SD_DeInit、关闭 SDIO clock、GPIO 退出 AF12；
     * 6. 恢复保存的 0x3018，让 OV5640 重新驱动 DVP；7. 恢复 DCMI/DMA 及原连续采集。
     * RecordFirstError 保留最早根因，但后续清理仍全部执行，避免错误覆盖或资源残留。
     */
    cleanup_start_tick = HAL_GetTick();
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
        // SDIO 尚可访问时卸载，不能等到 DeInit/关时钟后再调用 FatFs。
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
    result_info.cleanup_ms = HAL_GetTick() - cleanup_start_tick;
    result_info.total_ms = HAL_GetTick() - total_start_tick;
    s_camera_sd_status.last_total_ms = result_info.total_ms;
    s_camera_sd_status.last_write_ms = result_info.write_ms;
    if (result == CAMERA_SD_OK)
    {
        (void)memcpy(
            s_camera_sd_last_file_name,
            s_camera_sd_candidate_file_name,
            sizeof(s_camera_sd_last_file_name));
        s_camera_sd_status.last_snapshot_text = "PASS";
        s_camera_sd_status.last_file_name = s_camera_sd_last_file_name;
        s_camera_sd_status.last_file_size = file_bytes_written;
        result_info.file_name = s_camera_sd_last_file_name;
        ++s_camera_sd_status.save_count;
    }
    else
    {
        s_camera_sd_status.last_snapshot_text = "FAIL";
    }

    s_camera_sd_status.card_ready =
        (result == CAMERA_SD_OK) ? 1U : 0U;
    Camera_SDStorage_SetSaveError(result);
    Camera_SDStorage_SetLastError(result);
    result_info.bytes_written =
        (result == CAMERA_SD_OK) ? file_bytes_written : 0U;
    result_info.error_code = result;
    result_info.error_text = Camera_SDStorage_ErrorToString(result);
    *prepared_result = result_info;
    return result;
}

// 将 SD 存储错误码转换为稳定的状态文本
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
        case CAMERA_SD_ERR_FILE_SCAN_FAILED:
            return "FILE_SCAN_FAILED";
        case CAMERA_SD_ERR_FILE_INDEX_FULL:
            return "FILE_INDEX_FULL";
        default:
            return "UNKNOWN_ERROR";
    }
}
