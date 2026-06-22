#include "camera_pc_dump.h"

#define PC_DUMP_UART_CHUNK_SIZE  1024U

static uint32_t s_pc_dump_frame_buffer[PC_DUMP_WORD_COUNT] __attribute__((aligned(4)));

static void Camera_PC_Dump_WriteU16LE(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void Camera_PC_Dump_WriteU32LE(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

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

uint32_t Camera_PC_Dump_GetBufferAddress(void)
{
    return (uint32_t)s_pc_dump_frame_buffer;
}

uint32_t Camera_PC_Dump_GetWordCount(void)
{
    return PC_DUMP_WORD_COUNT;
}

uint8_t Camera_PC_Dump_WaitForDumpCommand(UART_HandleTypeDef *huart)
{
    uint8_t line[8];
    uint8_t line_length = 0U;
    uint8_t byte;

    if (huart == NULL)
    {
        return 0U;
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

        if (byte == '\r')
        {
            continue;
        }

        if (byte == '\n')
        {
            if ((line_length == 4U) &&
                (line[0] == 'D') &&
                (line[1] == 'U') &&
                (line[2] == 'M') &&
                (line[3] == 'P'))
            {
                return 1U;
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

uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id)
{
    static const uint8_t magic[8] = {'O', 'V', '5', '6', 'R', 'G', 'B', '5'};
    uint8_t header[22];
    uint8_t crc_bytes[4];
    const uint8_t *payload = (const uint8_t *)s_pc_dump_frame_buffer;
    uint32_t offset = 0U;
    uint32_t crc;

    if (huart == NULL)
    {
        return 1U;
    }

    for (uint32_t i = 0U; i < sizeof(magic); ++i)
    {
        header[i] = magic[i];
    }

    header[8] = 1U;
    header[9] = 1U;
    Camera_PC_Dump_WriteU16LE(&header[10], (uint16_t)PC_DUMP_WIDTH);
    Camera_PC_Dump_WriteU16LE(&header[12], (uint16_t)PC_DUMP_HEIGHT);
    Camera_PC_Dump_WriteU32LE(&header[14], PC_DUMP_PAYLOAD_LEN);
    Camera_PC_Dump_WriteU32LE(&header[18], frame_id);

    crc = Camera_PC_Dump_CRC32(payload, PC_DUMP_PAYLOAD_LEN);
    Camera_PC_Dump_WriteU32LE(crc_bytes, crc);

    if (HAL_UART_Transmit(huart, header, sizeof(header), HAL_MAX_DELAY) != HAL_OK)
    {
        return 2U;
    }

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

    if (HAL_UART_Transmit(huart, crc_bytes, sizeof(crc_bytes), HAL_MAX_DELAY) != HAL_OK)
    {
        return 4U;
    }

    return 0U;
}
