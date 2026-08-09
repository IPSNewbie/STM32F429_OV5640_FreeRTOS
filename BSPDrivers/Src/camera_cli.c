#include "camera_cli.h"

#include "camera_frame_buffer.h"
#include "camera_rtos.h"
#include "camera_sd_storage.h"
#include "camera_snapshot_control.h"
#include "uart_rx_dma.h"

#include <stddef.h>

#define CAMERA_CLI_DEFAULT_THRESHOLD 128U

static CameraCliRuntimeConfig_t s_camera_cli_config;

static void Camera_CLI_WriteText(UART_HandleTypeDef *huart, const char *text)
{
    const char *cursor = text;
    uint16_t length = 0U;

    if ((huart == NULL) || (text == NULL))
    {
        return;
    }

    while (*cursor != '\0')
    {
        ++cursor;
        ++length;
    }

    if (length != 0U)
    {
        (void)HAL_UART_Transmit(
            huart,
            (uint8_t *)text,
            length,
            HAL_MAX_DELAY);
    }
}

static void Camera_CLI_WriteLine(UART_HandleTypeDef *huart, const char *text)
{
    Camera_CLI_WriteText(huart, text);
    Camera_CLI_WriteText(huart, "\r\n");
}

static void Camera_CLI_WriteU32(UART_HandleTypeDef *huart, uint32_t value)
{
    char buffer[11];
    uint32_t position = sizeof(buffer);

    buffer[--position] = '\0';
    do
    {
        buffer[--position] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (position > 0U));

    Camera_CLI_WriteText(huart, &buffer[position]);
}

static void Camera_CLI_WriteFieldU32(
    UART_HandleTypeDef *huart,
    const char *name,
    uint32_t value)
{
    Camera_CLI_WriteText(huart, "  ");
    Camera_CLI_WriteText(huart, name);
    Camera_CLI_WriteText(huart, "=");
    Camera_CLI_WriteU32(huart, value);
    Camera_CLI_WriteText(huart, "\r\n");
}

static void Camera_CLI_WriteFieldText(
    UART_HandleTypeDef *huart,
    const char *name,
    const char *value)
{
    Camera_CLI_WriteText(huart, "  ");
    Camera_CLI_WriteText(huart, name);
    Camera_CLI_WriteText(huart, "=");
    Camera_CLI_WriteLine(huart, value);
}

static char Camera_CLI_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - ('a' - 'A'));
    }

    return ch;
}

static uint8_t Camera_CLI_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U;
}

static const char *Camera_CLI_TrimLeft(const char *text)
{
    while ((text != NULL) && (Camera_CLI_IsSpace(*text) != 0U))
    {
        ++text;
    }

    return text;
}

static uint32_t Camera_CLI_TrimmedLength(const char *text)
{
    uint32_t length = 0U;

    if (text == NULL)
    {
        return 0U;
    }

    while (text[length] != '\0')
    {
        ++length;
    }

    while ((length > 0U) &&
           (Camera_CLI_IsSpace(text[length - 1U]) != 0U))
    {
        --length;
    }

    return length;
}

static uint8_t Camera_CLI_TokenEquals(
    const char *text,
    uint32_t length,
    const char *token)
{
    uint32_t index = 0U;

    while (token[index] != '\0')
    {
        if ((index >= length) ||
            (Camera_CLI_ToUpper(text[index]) != token[index]))
        {
            return 0U;
        }

        ++index;
    }

    return (index == length) ? 1U : 0U;
}

static uint8_t Camera_CLI_ParseU8(
    const char *text,
    uint32_t length,
    uint8_t *value)
{
    uint32_t parsed = 0U;
    uint32_t index;

    if ((text == NULL) || (value == NULL) || (length == 0U))
    {
        return 0U;
    }

    for (index = 0U; index < length; ++index)
    {
        if ((text[index] < '0') || (text[index] > '9'))
        {
            return 0U;
        }

        parsed = (parsed * 10U) + (uint32_t)(text[index] - '0');
        if (parsed > 255U)
        {
            return 0U;
        }
    }

    *value = (uint8_t)parsed;
    return 1U;
}

static const char *Camera_CLI_ModeName(CameraProcessMode_t mode)
{
    if (mode == CAMERA_PROCESS_MODE_BYPASS)
    {
        return "BYPASS";
    }
    if (mode == CAMERA_PROCESS_MODE_GRAYSCALE)
    {
        return "GRAYSCALE";
    }
    if (mode == CAMERA_PROCESS_MODE_BINARY)
    {
        return "BINARY";
    }

    return "UNKNOWN";
}

static void Camera_CLI_PrintHelp(UART_HandleTypeDef *huart)
{
    Camera_CLI_WriteLine(huart, "HELP");
    Camera_CLI_WriteLine(huart, "STATUS");
    Camera_CLI_WriteLine(huart, "PROC [BYPASS|GRAY|BINARY]");
    Camera_CLI_WriteLine(huart, "THR [0..255]");
    Camera_CLI_WriteLine(huart, "RESET");
    Camera_CLI_WriteLine(huart, "DUMP");
    Camera_CLI_WriteLine(huart, "SD STATUS");
    Camera_CLI_WriteLine(huart, "SD SNAPSHOT");
}

static void Camera_CLI_PrintStatus(UART_HandleTypeDef *huart)
{
    const CameraRtosStats_t *stats = Camera_RTOS_GetStats();
    const UartRxDmaStats_t *uart_stats = UART_RxDma_GetStats();

    Camera_CLI_WriteLine(huart, "STATUS:");
    Camera_CLI_WriteFieldText(
        huart,
        "mode",
        Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    Camera_CLI_WriteFieldU32(
        huart,
        "threshold",
        s_camera_cli_config.binary_threshold);
    Camera_CLI_WriteText(huart, "  frame=");
    Camera_CLI_WriteU32(huart, CAMERA_FB_WIDTH);
    Camera_CLI_WriteText(huart, "x");
    Camera_CLI_WriteU32(huart, CAMERA_FB_HEIGHT);
    Camera_CLI_WriteText(huart, "\r\n");
    Camera_CLI_WriteFieldText(
        huart,
        "tuning",
        "AEC_BASELINE,AWB_AUTO,B+1,C0,S1,SH0");
    Camera_CLI_WriteFieldU32(
        huart,
        "uptime_ms",
        (stats != NULL) ? stats->uptime_ms : 0U);

    Camera_CLI_WriteLine(huart, "HEALTH:");
    Camera_CLI_WriteFieldU32(
        huart,
        "heap_free",
        (stats != NULL) ? stats->free_heap_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "heap_min",
        (stats != NULL) ? stats->min_ever_free_heap_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_camera_min",
        (stats != NULL) ? stats->camera_service_stack_min_free_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_monitor_min",
        (stats != NULL) ? stats->monitor_stack_min_free_bytes : 0U);

    Camera_CLI_WriteLine(huart, "FAULT:");
    Camera_CLI_WriteFieldU32(
        huart,
        "hook_fault",
        (stats != NULL) ? stats->hook_fault_code : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "assert_line",
        (stats != NULL) ? stats->assert_line : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "uart_dma_error",
        (uart_stats != NULL) ? uart_stats->uart_error_count : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stream_overflow",
        (uart_stats != NULL) ? uart_stats->stream_overflow_bytes : 0U);

    Camera_CLI_WriteLine(huart, "IWDG:");
    Camera_CLI_WriteFieldU32(
        huart,
        "enabled",
        (stats != NULL) ? stats->iwdg_enabled : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "refresh_skip",
        (stats != NULL) ? stats->iwdg_refresh_skip_count : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "last_skip_reason",
        (stats != NULL) ? stats->iwdg_last_skip_reason : 0U);
}

static void Camera_CLI_PrintSdStatus(UART_HandleTypeDef *huart)
{
    CameraSdStorageStatus_t status;

    Camera_SDStorage_GetStatus(&status);
    Camera_CLI_WriteLine(huart, "SD STATUS:");
    Camera_CLI_WriteFieldU32(huart, "supported", status.supported);
    Camera_CLI_WriteFieldU32(huart, "card_ready", status.card_ready);
    Camera_CLI_WriteFieldU32(
        huart,
        "takeover_required",
        status.takeover_required);
    Camera_CLI_WriteFieldU32(huart, "sdio_ready", status.sdio_ready);
    Camera_CLI_WriteFieldU32(huart, "fatfs_ready", status.fatfs_ready);
    Camera_CLI_WriteFieldText(huart, "last_mount", status.last_mount_text);
    Camera_CLI_WriteFieldText(
        huart,
        "last_snapshot",
        status.last_snapshot_text);
    Camera_CLI_WriteFieldText(huart, "last_file", status.last_file_name);
    Camera_CLI_WriteFieldU32(
        huart,
        "last_file_size",
        status.last_file_size);
    Camera_CLI_WriteFieldU32(huart, "save_count", status.save_count);
    Camera_CLI_WriteFieldText(huart, "save_error", status.save_error_text);
    Camera_CLI_WriteFieldText(huart, "last_error", status.last_error_text);
    Camera_CLI_WriteFieldU32(huart, "last_total_ms", status.last_total_ms);
    Camera_CLI_WriteFieldU32(huart, "last_write_ms", status.last_write_ms);
    Camera_CLI_WriteFieldText(
        huart,
        "dvp_mask_solution",
        "OV5640_3018_6_4");
}

static CameraCliStatus_t Camera_CLI_RunSdSnapshot(
    UART_HandleTypeDef *huart)
{
    CameraSdSnapshotResult_t snapshot_result;
    uint32_t result;

    result = Camera_SDStorage_SaveSnapshotFrame(&snapshot_result);
    Camera_CLI_WriteLine(huart, "SD SNAPSHOT:");
    Camera_CLI_WriteFieldText(
        huart,
        "result",
        (result == CAMERA_SD_OK) ? "PASS" : "FAIL");
    Camera_CLI_WriteFieldText(huart, "file", snapshot_result.file_name);
    Camera_CLI_WriteFieldU32(
        huart,
        "bytes",
        snapshot_result.bytes_written);
    Camera_CLI_WriteFieldText(huart, "source", snapshot_result.source_text);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_bytes",
        snapshot_result.source_bytes);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_nonzero",
        snapshot_result.source_nonzero);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_sum32",
        snapshot_result.source_sum32);
    Camera_CLI_WriteFieldText(
        huart,
        "prepare",
        snapshot_result.prepare_text);
    Camera_CLI_WriteFieldU32(
        huart,
        "prepare_retry",
        snapshot_result.prepare_retry);
    Camera_CLI_WriteFieldText(huart, "format", snapshot_result.format_text);
    Camera_CLI_WriteFieldU32(huart, "width", snapshot_result.width);
    Camera_CLI_WriteFieldU32(huart, "height", snapshot_result.height);
    Camera_CLI_WriteFieldText(huart, "mount", snapshot_result.mount_text);
    Camera_CLI_WriteFieldText(huart, "write", snapshot_result.write_text);
    Camera_CLI_WriteFieldText(
        huart,
        "cleanup",
        snapshot_result.cleanup_text);
    Camera_CLI_WriteFieldText(
        huart,
        "restore",
        snapshot_result.restore_text);
    Camera_CLI_WriteFieldU32(huart, "total_ms", snapshot_result.total_ms);
    Camera_CLI_WriteFieldU32(huart, "prepare_ms", snapshot_result.prepare_ms);
    Camera_CLI_WriteFieldU32(huart, "write_ms", snapshot_result.write_ms);
    Camera_CLI_WriteFieldU32(huart, "cleanup_ms", snapshot_result.cleanup_ms);
    if (result != CAMERA_SD_OK)
    {
        Camera_CLI_WriteFieldText(
            huart,
            "error",
            snapshot_result.error_text);
        return CAMERA_CLI_ERROR;
    }

    return CAMERA_CLI_OK;
}

static CameraCliStatus_t Camera_CLI_HandleProc(
    UART_HandleTypeDef *huart,
    const char *argument,
    uint32_t argument_length)
{
    if (argument_length == 0U)
    {
        Camera_CLI_WriteText(huart, "process mode: ");
        Camera_CLI_WriteLine(
            huart,
            Camera_CLI_ModeName(s_camera_cli_config.process_mode));
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(argument, argument_length, "BYPASS") != 0U)
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    }
    else if ((Camera_CLI_TokenEquals(argument, argument_length, "GRAY") != 0U) ||
             (Camera_CLI_TokenEquals(argument, argument_length, "GRAYSCALE") != 0U))
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_GRAYSCALE;
    }
    else if (Camera_CLI_TokenEquals(argument, argument_length, "BINARY") != 0U)
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BINARY;
    }
    else
    {
        Camera_CLI_WriteLine(huart, "ERR bad PROC arg");
        return CAMERA_CLI_ERROR_BAD_ARG;
    }

    Camera_CLI_WriteText(huart, "OK process mode: ");
    Camera_CLI_WriteLine(
        huart,
        Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    return CAMERA_CLI_OK;
}

static CameraCliStatus_t Camera_CLI_HandleThreshold(
    UART_HandleTypeDef *huart,
    const char *argument,
    uint32_t argument_length)
{
    uint8_t threshold;

    if (argument_length == 0U)
    {
        Camera_CLI_WriteText(huart, "binary threshold: ");
        Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
        Camera_CLI_WriteText(huart, "\r\n");
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_ParseU8(argument, argument_length, &threshold) == 0U)
    {
        Camera_CLI_WriteLine(huart, "ERR bad THR arg");
        return CAMERA_CLI_ERROR_BAD_ARG;
    }

    s_camera_cli_config.binary_threshold = threshold;
    Camera_CLI_WriteText(huart, "OK binary threshold: ");
    Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
    Camera_CLI_WriteText(huart, "\r\n");
    return CAMERA_CLI_OK;
}

void Camera_CLI_ResetDefault(void)
{
    s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    s_camera_cli_config.binary_threshold = CAMERA_CLI_DEFAULT_THRESHOLD;
}

void Camera_CLI_Init(void)
{
    Camera_CLI_ResetDefault();
    Camera_SDStorage_InitState();
    Camera_SnapshotControl_InitState();
}

CameraProcessMode_t Camera_CLI_GetProcessMode(void)
{
    return s_camera_cli_config.process_mode;
}

uint8_t Camera_CLI_GetBinaryThreshold(void)
{
    return s_camera_cli_config.binary_threshold;
}

const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void)
{
    return &s_camera_cli_config;
}

CameraCliStatus_t Camera_CLI_HandleLine(
    UART_HandleTypeDef *huart,
    const char *line)
{
    const char *trimmed;
    const char *argument;
    uint32_t length;
    uint32_t command_length = 0U;
    uint32_t argument_length;

    if ((huart == NULL) || (line == NULL))
    {
        return CAMERA_CLI_ERROR_NULL;
    }

    trimmed = Camera_CLI_TrimLeft(line);
    length = Camera_CLI_TrimmedLength(trimmed);
    if (length == 0U)
    {
        return CAMERA_CLI_OK;
    }

    while ((command_length < length) &&
           (Camera_CLI_IsSpace(trimmed[command_length]) == 0U))
    {
        ++command_length;
    }

    argument = Camera_CLI_TrimLeft(&trimmed[command_length]);
    argument_length = Camera_CLI_TrimmedLength(argument);

    if (Camera_CLI_TokenEquals(trimmed, command_length, "HELP") != 0U)
    {
        Camera_CLI_PrintHelp(huart);
        return CAMERA_CLI_OK;
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "STATUS") != 0U)
    {
        Camera_CLI_PrintStatus(huart);
        return CAMERA_CLI_OK;
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "PROC") != 0U)
    {
        return Camera_CLI_HandleProc(huart, argument, argument_length);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "THR") != 0U)
    {
        return Camera_CLI_HandleThreshold(huart, argument, argument_length);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "RESET") != 0U)
    {
        Camera_CLI_ResetDefault();
        Camera_CLI_WriteLine(huart, "OK reset");
        return CAMERA_CLI_OK;
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "SD") != 0U)
    {
        if (Camera_CLI_TokenEquals(argument, argument_length, "STATUS") != 0U)
        {
            Camera_CLI_PrintSdStatus(huart);
            return CAMERA_CLI_OK;
        }
        if (Camera_CLI_TokenEquals(
                argument,
                argument_length,
                "SNAPSHOT") != 0U)
        {
            return Camera_CLI_RunSdSnapshot(huart);
        }

        Camera_CLI_WriteLine(huart, "ERR unknown command");
        return CAMERA_CLI_ERROR_UNKNOWN_CMD;
    }

    Camera_CLI_WriteLine(huart, "ERR unknown command");
    return CAMERA_CLI_ERROR_UNKNOWN_CMD;
}
