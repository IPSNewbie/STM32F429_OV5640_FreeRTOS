#include "camera_cli.h"
#include "camera_frame_buffer.h"

#include <stddef.h>

// 默认二值化阈值（128）
#define CAMERA_CLI_DEFAULT_THRESHOLD  128U

// 静态变量：保存当前 CLI 运行时配置（处理模式 + 二值化阈值）
static CameraCliRuntimeConfig_t s_camera_cli_config;

// 通过 UART 发送字符串（不自动换行）
static void Camera_CLI_WriteText(UART_HandleTypeDef *huart, const char *text)
{
    const char *p = text;
    uint16_t len = 0U;

    if ((huart == NULL) || (text == NULL))
    {
        return;  // 参数无效直接返回
    }

    // 计算字符串长度
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

// 将小写字母转换为大写（仅处理 a-z）
static char Camera_CLI_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

// 判断字符是否为空格或制表符
static uint8_t Camera_CLI_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U;
}

// 去除字符串左侧的空白字符
static const char *Camera_CLI_TrimLeft(const char *line)
{
    while ((line != NULL) && (Camera_CLI_IsSpace(*line) != 0U))
    {
        ++line;
    }
    return line;
}

// 计算去除首尾空白后的字符串长度（不修改原串）
static uint32_t Camera_CLI_TrimmedLength(const char *line)
{
    uint32_t len = 0U;

    if (line == NULL)
    {
        return 0U;
    }

    // 计算原始长度
    while (line[len] != '\0')
    {
        ++len;
    }

    // 从尾部去除空白字符
    while ((len > 0U) && (Camera_CLI_IsSpace(line[len - 1U]) != 0U))
    {
        --len;
    }

    return len;
}

// 比较文本（指定长度）是否与预定义令牌（大写）匹配（不区分大小写）
static uint8_t Camera_CLI_TokenEquals(const char *text,
                                      uint32_t len,
                                      const char *token)
{
    uint32_t i = 0U;

    while (token[i] != '\0')
    {
        if (i >= len)
        {
            return 0U;  // 文本长度不足
        }
        // 将文本字符转为大写后与 token 比较
        if (Camera_CLI_ToUpper(text[i]) != token[i])
        {
            return 0U;
        }
        ++i;
    }

    return (i == len) ? 1U : 0U;  // 长度完全匹配才成功
}

// 将处理模式枚举转换为可读字符串
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

// 通过 UART 发送一个 32 位无符号整数（十进制）
static void Camera_CLI_WriteU32(UART_HandleTypeDef *huart, uint32_t value)
{
    char buf[11];
    uint32_t pos = sizeof(buf);

    buf[--pos] = '\0';  // 字符串终止符

    // 从个位开始逐位转换为字符
    do
    {
        buf[--pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (pos > 0U));

    Camera_CLI_WriteText(huart, &buf[pos]);
}

// 通过 UART 发送字符串并自动追加换行（\r\n）
static void Camera_CLI_WriteLine(UART_HandleTypeDef *huart, const char *text)
{
    Camera_CLI_WriteText(huart, text);
    Camera_CLI_WriteText(huart, "\r\n");
}

// 解析字符串（指定长度）为 8 位无符号整数（0~255）
static uint8_t Camera_CLI_ParseU8(const char *text, uint32_t len, uint8_t *value)
{
    uint32_t parsed = 0U;

    if ((text == NULL) || (value == NULL) || (len == 0U))
    {
        return 0U;
    }

    for (uint32_t i = 0U; i < len; ++i)
    {
        // 检查是否数字字符
        if ((text[i] < '0') || (text[i] > '9'))
        {
            return 0U;
        }
        parsed = (parsed * 10U) + (uint32_t)(text[i] - '0');
        if (parsed > 255U)
        {
            return 0U;  // 超出 0-255 范围
        }
    }

    *value = (uint8_t)parsed;
    return 1U;  // 解析成功
}

// 将配置重置为默认值（旁路模式 + 默认阈值）
void Camera_CLI_ResetDefault(void)
{
    s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    s_camera_cli_config.binary_threshold = CAMERA_CLI_DEFAULT_THRESHOLD;
}

// 初始化 CLI 模块（调用重置默认值）
void Camera_CLI_Init(void)
{
    Camera_CLI_ResetDefault();
}

// 获取当前处理模式
CameraProcessMode_t Camera_CLI_GetProcessMode(void)
{
    return s_camera_cli_config.process_mode;
}

// 获取当前二值化阈值
uint8_t Camera_CLI_GetBinaryThreshold(void)
{
    return s_camera_cli_config.binary_threshold;
}

// 获取当前配置结构的只读指针
const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void)
{
    return &s_camera_cli_config;
}

// 打印帮助信息（支持的命令列表）
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

// 打印当前完整状态（处理模式、阈值、AEC/AWB 设置、图像尺寸等）
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

// 处理 "PROC" 命令（显示/设置处理模式）
static CameraCliStatus_t Camera_CLI_HandleProc(UART_HandleTypeDef *huart,
                                               const char *arg,
                                               uint32_t arg_len)
{
    if (arg_len == 0U)
    {
        // 无参数：显示当前模式
        Camera_CLI_WriteText(huart, "process mode: ");
        Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode));
        return CAMERA_CLI_OK;
    }

    // 根据参数设置模式
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

    // 确认设置
    Camera_CLI_WriteText(huart, "OK process mode: ");
    Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    return CAMERA_CLI_OK;
}

// 处理 "THR" 命令（显示/设置二值化阈值）
static CameraCliStatus_t Camera_CLI_HandleThreshold(UART_HandleTypeDef *huart,
                                                    const char *arg,
                                                    uint32_t arg_len)
{
    uint8_t threshold;

    if (arg_len == 0U)
    {
        // 无参数：显示当前阈值
        Camera_CLI_WriteText(huart, "binary threshold: ");
        Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
        Camera_CLI_WriteText(huart, "\r\n");
        return CAMERA_CLI_OK;
    }

    // 解析参数为 0~255
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

// 解析并执行一行用户命令（主入口）
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

    // 去除首尾空白
    trimmed = Camera_CLI_TrimLeft(line);
    len = Camera_CLI_TrimmedLength(trimmed);
    if (len == 0U)
    {
        return CAMERA_CLI_OK;  // 空行直接忽略
    }

    // 提取命令部分（第一个单词）
    while ((cmd_len < len) && (Camera_CLI_IsSpace(trimmed[cmd_len]) == 0U))
    {
        ++cmd_len;
    }

    // 提取参数部分（剩余内容）
    arg = Camera_CLI_TrimLeft(&trimmed[cmd_len]);
    arg_len = Camera_CLI_TrimmedLength(arg);

    // 命令匹配（不区分大小写）
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

    // 未知命令
    Camera_CLI_WriteLine(huart, "ERR unknown command");
    return CAMERA_CLI_ERROR_UNKNOWN_CMD;
}
