#include "protocol_crc32.h"

#define PROTOCOL_CRC32_INITIAL_VALUE        0xFFFFFFFFU
#define PROTOCOL_CRC32_REFLECTED_POLYNOMIAL 0xEDB88320U
#define PROTOCOL_CRC32_FINAL_XOR_VALUE      0xFFFFFFFFU

uint32_t Protocol_CRC32_Init(void)
{
    return PROTOCOL_CRC32_INITIAL_VALUE;
}

uint32_t Protocol_CRC32_UpdateByte(uint32_t crc, uint8_t data)
{
    return Protocol_CRC32_Update(crc, &data, 1U);
}

uint32_t Protocol_CRC32_Update(uint32_t crc,
                               const uint8_t *data,
                               size_t length)
{
    size_t i;
    uint32_t bit;

    // 非空长度遇到空指针时保持当前状态，避免解引用空指针
    if (data == NULL)
    {
        return crc;
    }

    for (i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            // 保持原有逐位反射算法，不引入查找表或硬件 CRC 外设
            crc = (crc >> 1) ^
                  ((0U - (crc & 1U)) & PROTOCOL_CRC32_REFLECTED_POLYNOMIAL);
        }
    }

    return crc;
}

uint32_t Protocol_CRC32_Finalize(uint32_t crc)
{
    return crc ^ PROTOCOL_CRC32_FINAL_XOR_VALUE;
}

uint32_t Protocol_CRC32_Calculate(const uint8_t *data, size_t length)
{
    uint32_t crc = Protocol_CRC32_Init();

    crc = Protocol_CRC32_Update(crc, data, length);
    return Protocol_CRC32_Finalize(crc);
}
