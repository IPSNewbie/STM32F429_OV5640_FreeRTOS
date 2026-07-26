#include "camera_pc_dump.h"
#include "camera_cli.h"
#include "camera_frame_buffer.h"

#include <string.h>

//============================================================================
// @file    camera_pc_dump.c
// @brief   OV5640 图像帧 UART 导出模块
//          用于将摄像头采集的 RGB565 图像帧通过 UART 发送到 PC，
//          配合上位机工具进行实时显示或图像分析。
//============================================================================

// UART 每次发送的最大字节数（1024 字节），避免长时间占用总线
#define PC_DUMP_UART_CHUNK_SIZE  1024U

// 用于接收串口命令行的最大长度（32 字节，含终止符）
#define PC_DUMP_COMMAND_LINE_LEN 32U

static char s_camera_pc_dump_line[PC_DUMP_COMMAND_LINE_LEN];
static uint8_t s_camera_pc_dump_line_length;
static uint8_t s_camera_pc_dump_line_overflow;
static UART_HandleTypeDef *s_camera_pc_dump_command_uart;

// 将小写字母转换为大写（仅处理 a-z）
static char Camera_PC_Dump_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

// 判断字符是否为空格或制表符
static uint8_t Camera_PC_Dump_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U;
}

// 去除字符串左侧的空白字符
static const char *Camera_PC_Dump_TrimLeft(const char *line)
{
    while ((line != NULL) && (Camera_PC_Dump_IsSpace(*line) != 0U))
    {
        ++line;
    }
    return line;
}

// 计算去除首尾空白后的字符串长度
static uint32_t Camera_PC_Dump_TrimmedLength(const char *line)
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
    while ((len > 0U) && (Camera_PC_Dump_IsSpace(line[len - 1U]) != 0U))
    {
        --len;
    }
    return len;
}

// 检查整行字符串（去除空白后）是否与给定的令牌（大写）匹配（不区分大小写）
static uint8_t Camera_PC_Dump_LineEquals(const char *line, const char *token)
{
    const char *trimmed = Camera_PC_Dump_TrimLeft(line);
    uint32_t len = Camera_PC_Dump_TrimmedLength(trimmed);
    uint32_t i = 0U;

    while (token[i] != '\0')
    {
        if (i >= len)
        {
            return 0U;  // 长度不足
        }
        if (Camera_PC_Dump_ToUpper(trimmed[i]) != token[i])
        {
            return 0U;  // 字符不匹配
        }
        ++i;
    }
    return (i == len) ? 1U : 0U;  // 完全匹配
}

// 清除 UART 可能存在的各种错误标志（PE, FE, NE, ORE）
static void Camera_PC_Dump_ClearUartErrors(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        return;
    }

#ifdef UART_FLAG_PE
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_PE) != RESET)
    {
        __HAL_UART_CLEAR_PEFLAG(huart);
    }
#endif
#ifdef UART_FLAG_FE
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_FE) != RESET)
    {
        __HAL_UART_CLEAR_FEFLAG(huart);
    }
#endif
#ifdef UART_FLAG_NE
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_NE) != RESET)
    {
        __HAL_UART_CLEAR_NEFLAG(huart);
    }
#endif
#ifdef UART_FLAG_ORE
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
    }
#endif

    huart->ErrorCode = HAL_UART_ERROR_NONE;  // 重置错误码
}

// 重置命令行缓存（清空缓冲区，重置长度和溢出标志）
static void Camera_PC_Dump_ResetLine(char *line,
                                     uint32_t line_size,
                                     uint8_t *line_length,
                                     uint8_t *line_overflow)
{
    if ((line != NULL) && (line_size > 0U))
    {
        (void)memset(line, 0, line_size);  // 清空缓存
    }
    if (line_length != NULL)
    {
        *line_length = 0U;
    }
    if (line_overflow != NULL)
    {
        *line_overflow = 0U;
    }
}

// 将 uint16_t 以小端序写入字节数组
static void Camera_PC_Dump_WriteU16LE(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

// 将 uint32_t 以小端序写入字节数组
static void Camera_PC_Dump_WriteU32LE(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

// 计算 CRC32（标准多项式 0xEDB88320），用于数据校验
static uint32_t Camera_PC_Dump_CRC32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;   // 初始值
    for (uint32_t i = 0U; i < length; ++i)
    {
        crc ^= data[i];           // 与当前字节异或
        for (uint32_t bit = 0U; bit < 8U; ++bit)
        {
            // 标准位运算：若最低位为 1，则右移后异或多项式
            crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xEDB88320U);
        }
    }
    return crc ^ 0xFFFFFFFFU;     // 取反输出
}

// 获取图像帧缓冲区的 32 位起始地址，用于配置 DCMI DMA 的目标地址
uint32_t Camera_PC_Dump_GetBufferAddress(void)
{
    return (uint32_t)Camera_FrameBuffer_GetBackBuffer();
}

// 获取图像帧缓冲区的字数（32 位为单位），用于配置 DCMI DMA 的传输数量
uint32_t Camera_PC_Dump_GetWordCount(void)
{
    return PC_DUMP_WORD_COUNT;
}

// 轮询方式接收 PC 串口命令（单字节有限超时版本）
// 参数：huart - UART 句柄
// 返回值：
//   CAMERA_PC_DUMP_CMD_DUMP   - 收到完整 "DUMP" 命令
//   CAMERA_PC_DUMP_CMD_CLI    - 收到其他完整命令行（已交由 CLI 处理）
//   CAMERA_PC_DUMP_CMD_PENDING - 正在接收命令（尚未完整）
//   CAMERA_PC_DUMP_CMD_NONE   - 无数据或错误
// 说明：使用静态变量缓存当前行状态，支持逐字节组装，行结束符为 '\n' 或 '\r'。
uint8_t Camera_PC_Dump_PollCommand(UART_HandleTypeDef *huart)
{
    uint8_t byte;
    HAL_StatusTypeDef status;

    // 无效句柄直接返回无命令
    if (huart == NULL)
    {
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    // 若 UART 句柄发生变化，重置状态（用于切换串口）
    if (s_camera_pc_dump_command_uart != huart)
    {
        s_camera_pc_dump_command_uart = huart;
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 &s_camera_pc_dump_line_overflow);
    }

    // 尝试接收 1 字节。不要在接收前清 UART 错误，避免误丢正常字节。
    status = HAL_UART_Receive(huart, &byte, 1U, 1U);
    if (status == HAL_TIMEOUT)
    {
        // 超时表示当前无新数据，返回“等待中”
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    if (status == HAL_ERROR)
    {
        // 接收出错，清除错误并重置行缓存，返回无命令
        Camera_PC_Dump_ClearUartErrors(huart);
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 &s_camera_pc_dump_line_overflow);
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    if (status != HAL_OK)
    {
        // HAL_BUSY 等非错误状态不清 UART 错误，也不丢弃已接收的行缓存
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    // 处理行结束符（\r 或 \n）
    if ((byte == '\r') || (byte == '\n'))
    {
        // 如果之前发生了行溢出，告知 CLI 有未知命令并重置
        if (s_camera_pc_dump_line_overflow != 0U)
        {
            (void)Camera_CLI_HandleLine(huart, "UNKNOWN");
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);
            return CAMERA_PC_DUMP_CMD_CLI;
        }

        // 空行忽略，返回“等待中”
        if (s_camera_pc_dump_line_length == 0U)
        {
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);
            return CAMERA_PC_DUMP_CMD_PENDING;
        }

        // 添加字符串结束符
        s_camera_pc_dump_line[s_camera_pc_dump_line_length] = '\0';

        // 检查是否为 "DUMP" 命令
        if (Camera_PC_Dump_LineEquals(s_camera_pc_dump_line, "DUMP") != 0U)
        {
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);
            return CAMERA_PC_DUMP_CMD_DUMP;
        }

        // 其他命令交给 CLI 模块处理
        (void)Camera_CLI_HandleLine(huart, s_camera_pc_dump_line);
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 &s_camera_pc_dump_line_overflow);
        return CAMERA_PC_DUMP_CMD_CLI;
    }

    // 过滤不可打印字符（保留制表符，删除 0x7F 及小于 0x20 的字符）
    if (((byte < 0x20U) && (byte != '\t')) || (byte == 0x7FU))
    {
        return CAMERA_PC_DUMP_CMD_PENDING;
    }

    // 如果之前已溢出，则丢弃后续字符直到行结束
    if (s_camera_pc_dump_line_overflow != 0U)
    {
        return CAMERA_PC_DUMP_CMD_PENDING;
    }

    // 将有效字符存入缓存（留一个位置给终止符）
    if (s_camera_pc_dump_line_length < (uint8_t)(sizeof(s_camera_pc_dump_line) - 1U))
    {
        s_camera_pc_dump_line[s_camera_pc_dump_line_length++] = (char)byte;
    }
    else
    {
        // 缓存已满，标记溢出并重置长度（保留已接收内容，但后续将丢弃直至行结束）
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 NULL);
        s_camera_pc_dump_line_overflow = 1U;
    }

    // 尚未收到行结束符，返回“等待中”
    return CAMERA_PC_DUMP_CMD_PENDING;
}

// 将一帧图像数据打包并通过 UART 发送给 PC
uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id)
{
    static const uint8_t magic[8] = {'O', 'V', '5', '6', 'R', 'G', 'B', '5'};  // 固定魔数
    uint8_t header[22];             // 帧头缓冲区（8+1+1+2+2+4+4 = 22 字节）
    uint8_t crc_bytes[4];           // CRC 存储（小端）
    CameraFrame_t frame;            // 当前前台帧信息
    const uint8_t *payload;         // 指向图像数据的指针
    uint32_t offset = 0U;           // 已发送字节偏移
    uint32_t crc;                   // 计算出的 CRC32

    if (huart == NULL)
    {
        return 1U;
    }

    // 获取前台帧，并校验尺寸与预期一致
    if ((Camera_FrameBuffer_GetFrontFrame(&frame) != CAMERA_FB_OK) ||
        (frame.data == NULL) ||
        (frame.width != PC_DUMP_WIDTH) ||
        (frame.height != PC_DUMP_HEIGHT) ||
        (frame.size_bytes != PC_DUMP_PAYLOAD_LEN))
    {
        return 5U;
    }

    payload = (const uint8_t *)frame.data;

    // 组装帧头
    for (uint32_t i = 0U; i < sizeof(magic); ++i)
    {
        header[i] = magic[i];
    }
    header[8] = 1U;     // 版本号
    header[9] = 1U;     // 保留（图像格式标识）
    Camera_PC_Dump_WriteU16LE(&header[10], (uint16_t)PC_DUMP_WIDTH);   // 宽度
    Camera_PC_Dump_WriteU16LE(&header[12], (uint16_t)PC_DUMP_HEIGHT);  // 高度
    Camera_PC_Dump_WriteU32LE(&header[14], PC_DUMP_PAYLOAD_LEN);       // 有效载荷长度
    Camera_PC_Dump_WriteU32LE(&header[18], frame_id);                  // 帧序号

    // 计算有效载荷的 CRC32
    crc = Camera_PC_Dump_CRC32(payload, PC_DUMP_PAYLOAD_LEN);
    Camera_PC_Dump_WriteU32LE(crc_bytes, crc);

    // 发送帧头（22 字节）
    if (HAL_UART_Transmit(huart, header, sizeof(header), HAL_MAX_DELAY) != HAL_OK)
    {
        return 2U;
    }

    // 分块发送有效载荷（每块不超过 1024 字节）
    while (offset < PC_DUMP_PAYLOAD_LEN)
    {
        uint32_t remaining = PC_DUMP_PAYLOAD_LEN - offset;
        uint16_t chunk = (uint16_t)((remaining > PC_DUMP_UART_CHUNK_SIZE)
                                    ? PC_DUMP_UART_CHUNK_SIZE
                                    : remaining);
        if (HAL_UART_Transmit(huart,
                              (uint8_t *)&payload[offset],
                              chunk,
                              HAL_MAX_DELAY) != HAL_OK)
        {
            return 3U;
        }
        offset += chunk;
    }

    // 发送 CRC32 校验值（4 字节）
    if (HAL_UART_Transmit(huart, crc_bytes, sizeof(crc_bytes), HAL_MAX_DELAY) != HAL_OK)
    {
        return 4U;
    }

    return 0U;
}
