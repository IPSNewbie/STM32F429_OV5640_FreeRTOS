#include "camera_cli.h"
#include "camera_frame_buffer.h"

#include <stddef.h>

#define CAMERA_CLI_DEFAULT_THRESHOLD  128U

static CameraCliRuntimeConfig_t s_camera_cli_config;

static void Camera_CLI_WriteText(UART_HandleTypeDef *huart, const char *text)
{
    const char *p = text;
    uint16_t len = 0U;

    if ((huart == NULL) || (text == NULL))
    {
        return;
    }

    while (*p != '\0')
    {
        ++p;
        ++len;
    }

    if (len > 0U)
    {
        (void)HAL_UART_Transmit(huart, (uint8_t *)text, len, HAL_MAX_DELAY);
    }
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

static const char *Camera_CLI_TrimLeft(const char *line)
{
    while ((line != NULL) && (Camera_CLI_IsSpace(*line) != 0U))
    {
        ++line;
    }

    return line;
}

static uint32_t Camera_CLI_TrimmedLength(const char *line)
{
    uint32_t len = 0U;

    if (line == NULL)
    {
        return 0U;
    }

    while (line[len] != '\0')
    {
        ++len;
    }

    while ((len > 0U) && (Camera_CLI_IsSpace(line[len - 1U]) != 0U))
    {
        --len;
    }

    return len;
}

static uint8_t Camera_CLI_TokenEquals(const char *text,
                                      uint32_t len,
                                      const char *token)
{
    uint32_t i = 0U;

    while (token[i] != '\0')
    {
        if (i >= len)
        {
            return 0U;
        }

        if (Camera_CLI_ToUpper(text[i]) != token[i])
        {
            return 0U;
        }

        ++i;
    }

    return (i == len) ? 1U : 0U;
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

static void Camera_CLI_WriteU32(UART_HandleTypeDef *huart, uint32_t value)
{
    char buf[11];
    uint32_t pos = sizeof(buf);

    buf[--pos] = '\0';

    do
    {
        buf[--pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (pos > 0U));

    Camera_CLI_WriteText(huart, &buf[pos]);
}

static void Camera_CLI_WriteLine(UART_HandleTypeDef *huart, const char *text)
{
    Camera_CLI_WriteText(huart, text);
    Camera_CLI_WriteText(huart, "\r\n");
}

static uint8_t Camera_CLI_ParseU8(const char *text, uint32_t len, uint8_t *value)
{
    uint32_t parsed = 0U;

    if ((text == NULL) || (value == NULL) || (len == 0U))
    {
        return 0U;
    }

    for (uint32_t i = 0U; i < len; ++i)
    {
        if ((text[i] < '0') || (text[i] > '9'))
        {
            return 0U;
        }

        parsed = (parsed * 10U) + (uint32_t)(text[i] - '0');
        if (parsed > 255U)
        {
            return 0U;
        }
    }

    *value = (uint8_t)parsed;
    return 1U;
}

void Camera_CLI_ResetDefault(void)
{
    s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    s_camera_cli_config.binary_threshold = CAMERA_CLI_DEFAULT_THRESHOLD;
}

void Camera_CLI_Init(void)
{
    Camera_CLI_ResetDefault();
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

static void Camera_CLI_PrintHelp(UART_HandleTypeDef *huart)
{
    Camera_CLI_WriteLine(huart, "HELP");
    Camera_CLI_WriteLine(huart, "STATUS");
    Camera_CLI_WriteLine(huart, "PROC");
    Camera_CLI_WriteLine(huart, "PROC BYPASS");
    Camera_CLI_WriteLine(huart, "PROC GRAY");
    Camera_CLI_WriteLine(huart, "PROC BINARY");
    Camera_CLI_WriteLine(huart, "THR");
    Camera_CLI_WriteLine(huart, "THR 0..255");
    Camera_CLI_WriteLine(huart, "RESET");
    Camera_CLI_WriteLine(huart, "DUMP");
}

static void Camera_CLI_PrintStatus(UART_HandleTypeDef *huart)
{
    Camera_CLI_WriteText(huart, "process mode: ");
    Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    Camera_CLI_WriteText(huart, "binary threshold: ");
    Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
    Camera_CLI_WriteText(huart, "\r\n");
    Camera_CLI_WriteLine(huart, "AEC: OV5640_AEC_TARGET_BASELINE");
    Camera_CLI_WriteLine(huart, "AWB: OV5640_AWB_MODE_AUTO");
    Camera_CLI_WriteLine(huart, "image tuning: brightness=+1 contrast=0 saturation=1 sharpness=0");
    Camera_CLI_WriteText(huart, "frame size: ");
    Camera_CLI_WriteU32(huart, CAMERA_FB_WIDTH);
    Camera_CLI_WriteText(huart, "x");
    Camera_CLI_WriteU32(huart, CAMERA_FB_HEIGHT);
    Camera_CLI_WriteText(huart, "\r\n");
}

static CameraCliStatus_t Camera_CLI_HandleProc(UART_HandleTypeDef *huart,
                                               const char *arg,
                                               uint32_t arg_len)
{
    if (arg_len == 0U)
    {
        Camera_CLI_WriteText(huart, "process mode: ");
        Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode));
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "BYPASS") != 0U)
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    }
    else if ((Camera_CLI_TokenEquals(arg, arg_len, "GRAY") != 0U) ||
             (Camera_CLI_TokenEquals(arg, arg_len, "GRAYSCALE") != 0U))
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_GRAYSCALE;
    }
    else if (Camera_CLI_TokenEquals(arg, arg_len, "BINARY") != 0U)
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BINARY;
    }
    else
    {
        Camera_CLI_WriteLine(huart, "ERR bad PROC arg");
        return CAMERA_CLI_ERROR_BAD_ARG;
    }

    Camera_CLI_WriteText(huart, "OK process mode: ");
    Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    return CAMERA_CLI_OK;
}

static CameraCliStatus_t Camera_CLI_HandleThreshold(UART_HandleTypeDef *huart,
                                                    const char *arg,
                                                    uint32_t arg_len)
{
    uint8_t threshold;

    if (arg_len == 0U)
    {
        Camera_CLI_WriteText(huart, "binary threshold: ");
        Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
        Camera_CLI_WriteText(huart, "\r\n");
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_ParseU8(arg, arg_len, &threshold) == 0U)
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

CameraCliStatus_t Camera_CLI_HandleLine(UART_HandleTypeDef *huart, const char *line)
{
    const char *trimmed;
    uint32_t len;
    uint32_t cmd_len = 0U;
    const char *arg;
    uint32_t arg_len;

    if ((huart == NULL) || (line == NULL))
    {
        return CAMERA_CLI_ERROR_NULL;
    }

    trimmed = Camera_CLI_TrimLeft(line);
    len = Camera_CLI_TrimmedLength(trimmed);
    if (len == 0U)
    {
        return CAMERA_CLI_OK;
    }

    while ((cmd_len < len) && (Camera_CLI_IsSpace(trimmed[cmd_len]) == 0U))
    {
        ++cmd_len;
    }

    arg = Camera_CLI_TrimLeft(&trimmed[cmd_len]);
    arg_len = Camera_CLI_TrimmedLength(arg);

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "HELP") != 0U)
    {
        Camera_CLI_PrintHelp(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "STATUS") != 0U)
    {
        Camera_CLI_PrintStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "PROC") != 0U)
    {
        return Camera_CLI_HandleProc(huart, arg, arg_len);
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "THR") != 0U)
    {
        return Camera_CLI_HandleThreshold(huart, arg, arg_len);
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "RESET") != 0U)
    {
        Camera_CLI_ResetDefault();
        Camera_CLI_WriteLine(huart, "OK reset");
        return CAMERA_CLI_OK;
    }

    Camera_CLI_WriteLine(huart, "ERR unknown command");
    return CAMERA_CLI_ERROR_UNKNOWN_CMD;
}
