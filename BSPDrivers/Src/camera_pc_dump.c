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

// 等待 PC 通过 UART 发送命令行（阻塞，支持 DUMP 和 AEC 命令，其余交给 CLI 处理）
uint8_t Camera_PC_Dump_WaitForCommand(UART_HandleTypeDef *huart)
{
    char line[PC_DUMP_COMMAND_LINE_LEN];   // 行缓存
    uint8_t line_length = 0U;              // 当前已接收字符数
    uint8_t line_overflow = 0U;            // 溢出标志（行太长）
    uint8_t byte;                          // 接收到的单字节

    if (huart == NULL)
    {
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    // 重置状态
    Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
    Camera_PC_Dump_ClearUartErrors(huart);

    for (;;)  // 无限循环，直到收到有效命令
    {
        Camera_PC_Dump_ClearUartErrors(huart);          // 每次循环前清除错误
        HAL_StatusTypeDef status = HAL_UART_Receive(huart, &byte, 1U, 100U);

        if (status == HAL_TIMEOUT)
        {
            continue;   // 超时则继续等待
        }

        if (status != HAL_OK)  // 接收出错
        {
            Camera_PC_Dump_ClearUartErrors(huart);
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
            continue;
        }

        // 处理行结束符（\r 或 \n）
        if ((byte == '\r') || (byte == '\n'))
        {
            if (line_overflow != 0U)  // 如果之前行溢出，通知 CLI 并重置
            {
                (void)Camera_CLI_HandleLine(huart, "UNKNOWN");
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                continue;
            }

            if (line_length == 0U)  // 空行则忽略
            {
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                continue;
            }

            line[line_length] = '\0';  // 添加字符串终止符

            // 检查是否为 "DUMP" 命令
            if (Camera_PC_Dump_LineEquals(line, "DUMP") != 0U)
            {
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                return CAMERA_PC_DUMP_CMD_DUMP;
            }

            // 非 DUMP 命令（包括 AEC 和其他 CLI 命令）交给 CLI 模块处理
            (void)Camera_CLI_HandleLine(huart, line);
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
            continue;
        }

        // 过滤不可打印字符（保留制表符，删除 0x7F）
        if (((byte < 0x20U) && (byte != '\t')) || (byte == 0x7FU))
        {
            continue;
        }

        // 如果已溢出，则丢弃后续字符直到行结束
        if (line_overflow != 0U)
        {
            continue;
        }

        // 将字符存入缓存（留一个位置给终止符）
        if (line_length < (uint8_t)(sizeof(line) - 1U))
        {
            line[line_length++] = (char)byte;
        }
        else
        {
            // 缓存已满，标记溢出并丢弃
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, NULL);
            line_overflow = 1U;
        }
    }
}

// 等待 PC 发送 "DUMP" 命令（忽略 AEC 等命令，只对 DUMP 返回 1）
uint8_t Camera_PC_Dump_WaitForDumpCommand(UART_HandleTypeDef *huart)
{
    uint8_t command;
    do
    {
        command = Camera_PC_Dump_WaitForCommand(huart);
    } while (command == CAMERA_PC_DUMP_CMD_AEC);  // 若收到 AEC，则继续等待
    return (command == CAMERA_PC_DUMP_CMD_DUMP) ? 1U : 0U;
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