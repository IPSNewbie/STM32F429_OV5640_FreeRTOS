#include "camera_pc_dump.h"

/**
 * @file    camera_pc_dump.c
 * @brief   OV5640 图像帧 UART 导出模块
 *          用于将摄像头采集的 RGB565 图像帧通过 UART 发送到 PC，
 *          配合上位机工具进行实时显示或图像分析。
 */

/* UART 每次发送的最大字节数，避免长时间占用总线 */
#define PC_DUMP_UART_CHUNK_SIZE  1024U

/*
 * 存放一帧图像数据的全局缓冲区。
 * 类型为 uint32_t，与 DCMI DMA 的 WORD 对齐要求一致。
 * 使用 __attribute__((aligned(4))) 确保 4 字节对齐。
 * 大小 PC_DUMP_WORD_COUNT 由外部宏定义（通常 = 宽度 * 高度 / 2）。
 */
static uint32_t s_pc_dump_frame_buffer[PC_DUMP_WORD_COUNT] __attribute__((aligned(4)));

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
    return (uint32_t)s_pc_dump_frame_buffer;
}

//获取图像帧缓冲区的字数（32 位为单位）， 用于配置 DCMI DMA 的传输数量
uint32_t Camera_PC_Dump_GetWordCount(void)
{
    return PC_DUMP_WORD_COUNT;
}

//等待 PC 通过 UART 发送命令行
uint8_t Camera_PC_Dump_WaitForCommand(UART_HandleTypeDef *huart)
{
    uint8_t line[8];
    uint8_t line_length = 0U;
    uint8_t byte;

    if (huart == NULL)
    {
        return CAMERA_PC_DUMP_CMD_NONE;
    }

    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
    }

    for (;;)
    {
        HAL_StatusTypeDef status = HAL_UART_Receive(huart, &byte, 1U, 100U);

        if (status == HAL_TIMEOUT)
        {
            continue;
        }

        if (status != HAL_OK)
        {
            if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET)
            {
                __HAL_UART_CLEAR_OREFLAG(huart);
            }
            line_length = 0U;
            continue;
        }

        if ((byte == '\r') || (byte == '\n'))
        {
            if (line_length == 0U)
            {
                continue;
            }

            if ((line_length == 4U) &&
                (line[0] == 'D') &&
                (line[1] == 'U') &&
                (line[2] == 'M') &&
                (line[3] == 'P'))
            {
                return CAMERA_PC_DUMP_CMD_DUMP;
            }

            if ((line_length == 3U) &&
                (line[0] == 'A') &&
                (line[1] == 'E') &&
                (line[2] == 'C'))
            {
                return CAMERA_PC_DUMP_CMD_AEC;
            }

            line_length = 0U;
            continue;
        }

        if (line_length < sizeof(line))
        {
            line[line_length++] = byte;
        }
        else
        {
            line_length = 0U;
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
    const uint8_t *payload = (const uint8_t *)s_pc_dump_frame_buffer;
    uint32_t offset = 0U;
    uint32_t crc;

    if (huart == NULL)
    {
        return 1U;
    }

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
