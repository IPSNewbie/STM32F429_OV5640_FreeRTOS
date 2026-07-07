#include "camera_pc_dump.h"
#include "camera_cli.h"
#include "camera_frame_buffer.h"

#include <string.h>

/**
 * @file    camera_pc_dump.c
 * @brief   OV5640 图像帧 UART 导出模块
 *          用于将摄像头采集的 RGB565 图像帧通过 UART 发送到 PC，
 *          配合上位机工具进行实时显示或图像分析。
 */

/* UART 每次发送的最大字节数，避免长时间占用总线 */
#define PC_DUMP_UART_CHUNK_SIZE  1024U
#define PC_DUMP_COMMAND_LINE_LEN 32U

static char Camera_PC_Dump_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - ('a' - 'A'));
    }

    return ch;
}

static uint8_t Camera_PC_Dump_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U;
}

static const char *Camera_PC_Dump_TrimLeft(const char *line)
{
    while ((line != NULL) && (Camera_PC_Dump_IsSpace(*line) != 0U))
    {
        ++line;
    }

    return line;
}

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

static uint8_t Camera_PC_Dump_LineEquals(const char *line, const char *token)
{
    const char *trimmed = Camera_PC_Dump_TrimLeft(line);
    uint32_t len = Camera_PC_Dump_TrimmedLength(trimmed);
    uint32_t i = 0U;

    while (token[i] != '\0')
    {
        if (i >= len)
        {
            return 0U;
        }

        if (Camera_PC_Dump_ToUpper(trimmed[i]) != token[i])
        {
            return 0U;
        }

        ++i;
    }

    return (i == len) ? 1U : 0U;
}

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

    huart->ErrorCode = HAL_UART_ERROR_NONE;
}

static void Camera_PC_Dump_ResetLine(char *line,
                                     uint32_t line_size,
                                     uint8_t *line_length,
                                     uint8_t *line_overflow)
{
    if ((line != NULL) && (line_size > 0U))
    {
        (void)memset(line, 0, line_size);
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

/**
 * @brief  将 uint16_t 以小端序写入字节数组
 */
static void Camera_PC_Dump_WriteU16LE(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief  将 uint32_t 以小端序写入字节数组
 */
static void Camera_PC_Dump_WriteU32LE(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief  计算 CRC32（标准多项式 0xEDB88320），用于数据校验
 * @param  data   : 数据指针
 * @param  length : 数据长度（字节）
 * @return CRC32 校验值
 */
static uint32_t Camera_PC_Dump_CRC32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xEDB88320U);
        }
    }

    return crc ^ 0xFFFFFFFFU;
}

//获取图像帧缓冲区的 32 位起始地址，用于配置 DCMI DMA 的目标地址
uint32_t Camera_PC_Dump_GetBufferAddress(void)
{
    return (uint32_t)Camera_FrameBuffer_GetBackBuffer();
}

//获取图像帧缓冲区的字数（32 位为单位）， 用于配置 DCMI DMA 的传输数量
uint32_t Camera_PC_Dump_GetWordCount(void)
{
    return PC_DUMP_WORD_COUNT;
}

//等待 PC 通过 UART 发送命令行
uint8_t Camera_PC_Dump_WaitForCommand(UART_HandleTypeDef *huart)
{
    char line[PC_DUMP_COMMAND_LINE_LEN];
    uint8_t line_length = 0U;
    uint8_t line_overflow = 0U;
    uint8_t byte;

    if (huart == NULL)
    {
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
    Camera_PC_Dump_ClearUartErrors(huart);

    for (;;)
    {
        Camera_PC_Dump_ClearUartErrors(huart);
        HAL_StatusTypeDef status = HAL_UART_Receive(huart, &byte, 1U, 100U);

        if (status == HAL_TIMEOUT)
        {
            continue;
        }

        if (status != HAL_OK)
        {
            Camera_PC_Dump_ClearUartErrors(huart);
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
            continue;
        }

        if ((byte == '\r') || (byte == '\n'))
        {
            if (line_overflow != 0U)
            {
                (void)Camera_CLI_HandleLine(huart, "UNKNOWN");
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                continue;
            }

            if (line_length == 0U)
            {
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                continue;
            }

            line[line_length] = '\0';

            if (Camera_PC_Dump_LineEquals(line, "DUMP") != 0U)
            {
                Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
                return CAMERA_PC_DUMP_CMD_DUMP;
            }

            (void)Camera_CLI_HandleLine(huart, line);
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, &line_overflow);
            continue;
        }

        if (((byte < 0x20U) && (byte != '\t')) || (byte == 0x7FU))
        {
            continue;
        }

        if (line_overflow != 0U)
        {
            continue;
        }

        if (line_length < (uint8_t)(sizeof(line) - 1U))
        {
            line[line_length++] = (char)byte;
        }
        else
        {
            Camera_PC_Dump_ResetLine(line, sizeof(line), &line_length, NULL);
            line_overflow = 1U;
        }
    }
}

uint8_t Camera_PC_Dump_WaitForDumpCommand(UART_HandleTypeDef *huart)
{
    uint8_t command;

    do
    {
        command = Camera_PC_Dump_WaitForCommand(huart);
    } while (command == CAMERA_PC_DUMP_CMD_AEC);

    return (command == CAMERA_PC_DUMP_CMD_DUMP) ? 1U : 0U;
}

//将一帧图像数据打包并通过 UART 发送给 PC
uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id)
{
    // 帧头魔数
    static const uint8_t magic[8] = {'O', 'V', '5', '6', 'R', 'G', 'B', '5'};
    uint8_t header[22];             // 完整帧头 22 字节
    uint8_t crc_bytes[4];           // CRC 值存储
    CameraFrame_t frame;
    const uint8_t *payload;
    uint32_t offset = 0U;
    uint32_t crc;

    if (huart == NULL)
    {
        return 1U;
    }

    if ((Camera_FrameBuffer_GetFrontFrame(&frame) != CAMERA_FB_OK) ||
        (frame.data == NULL) ||
        (frame.width != PC_DUMP_WIDTH) ||
        (frame.height != PC_DUMP_HEIGHT) ||
        (frame.size_bytes != PC_DUMP_PAYLOAD_LEN))
    {
        return 5U;
    }

    payload = (const uint8_t *)frame.data;

    /* 组装帧头 */
    for (uint32_t i = 0U; i < sizeof(magic); ++i)
    {
        header[i] = magic[i];
    }

    header[8] = 1U;     // 版本
    header[9] = 1U;     // 保留
    Camera_PC_Dump_WriteU16LE(&header[10], (uint16_t)PC_DUMP_WIDTH);
    Camera_PC_Dump_WriteU16LE(&header[12], (uint16_t)PC_DUMP_HEIGHT);
    Camera_PC_Dump_WriteU32LE(&header[14], PC_DUMP_PAYLOAD_LEN);
    Camera_PC_Dump_WriteU32LE(&header[18], frame_id);

    // 计算有效载荷的 CRC32
    crc = Camera_PC_Dump_CRC32(payload, PC_DUMP_PAYLOAD_LEN);
    Camera_PC_Dump_WriteU32LE(crc_bytes, crc);

    /* 发送帧头 */
    if (HAL_UART_Transmit(huart, header, sizeof(header), HAL_MAX_DELAY) != HAL_OK)
    {
        return 2U;
    }

    /* 分块发送有效载荷，每块最大 PC_DUMP_UART_CHUNK_SIZE 字节 */
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

    /* 发送 CRC32 校验值 */
    if (HAL_UART_Transmit(huart, crc_bytes, sizeof(crc_bytes), HAL_MAX_DELAY) != HAL_OK)
    {
        return 4U;
    }

    return 0U;
}
