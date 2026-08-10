/*
 * 协议 CRC32 主机侧单元测试。
 *
 * 使用标准向量验证一次性、逐字节、分块、空数据和空指针保护路径结果一致。
 */
#include "protocol_crc32.h"

#include <stdio.h>

#define PROTOCOL_CRC32_TEST_EXPECTED       0xCBF43926U
#define PROTOCOL_CRC32_EMPTY_EXPECTED      0x00000000U

// 比较 CRC32 实际值与期望值并输出失败详情
static int Protocol_CRC32_Check(const char *name,
                                uint32_t actual,
                                uint32_t expected)
{
    if (actual == expected)
    {
        return 0;
    }

    (void)fprintf(stderr,
                  "%s 失败：实际值=0x%08lX，期望值=0x%08lX\n",
                  name,
                  (unsigned long)actual,
                  (unsigned long)expected);
    return 1;
}

// 运行 CRC32 全部标准向量和边界测试
int main(void)
{
    static const uint8_t test_data[] = "123456789";
    uint32_t calculate_crc;
    uint32_t byte_update_crc;
    uint32_t update_byte_crc;
    uint32_t split_crc;
    uint32_t empty_crc;
    uint32_t null_crc;
    size_t i;
    int failed = 0;

    calculate_crc = Protocol_CRC32_Calculate(test_data,
                                             sizeof(test_data) - 1U);

    // 使用分块接口逐字节更新
    byte_update_crc = Protocol_CRC32_Init();
    for (i = 0U; i < (sizeof(test_data) - 1U); ++i)
    {
        byte_update_crc = Protocol_CRC32_Update(byte_update_crc,
                                                &test_data[i],
                                                1U);
    }
    byte_update_crc = Protocol_CRC32_Finalize(byte_update_crc);

    // 单独验证逐字节接口
    update_byte_crc = Protocol_CRC32_Init();
    for (i = 0U; i < (sizeof(test_data) - 1U); ++i)
    {
        update_byte_crc = Protocol_CRC32_UpdateByte(update_byte_crc,
                                                    test_data[i]);
    }
    update_byte_crc = Protocol_CRC32_Finalize(update_byte_crc);

    split_crc = Protocol_CRC32_Init();
    split_crc = Protocol_CRC32_Update(split_crc, test_data, 4U);
    split_crc = Protocol_CRC32_Update(split_crc, &test_data[4], 5U);
    split_crc = Protocol_CRC32_Finalize(split_crc);

    empty_crc = Protocol_CRC32_Calculate(NULL, 0U);
    // 正长度空指针按公共接口约定安全地忽略更新
    null_crc = Protocol_CRC32_Calculate(NULL, 1U);

    failed |= Protocol_CRC32_Check("一次性计算",
                                   calculate_crc,
                                   PROTOCOL_CRC32_TEST_EXPECTED);
    failed |= Protocol_CRC32_Check("分块接口逐字节计算",
                                   byte_update_crc,
                                   PROTOCOL_CRC32_TEST_EXPECTED);
    failed |= Protocol_CRC32_Check("逐字节接口计算",
                                   update_byte_crc,
                                   PROTOCOL_CRC32_TEST_EXPECTED);
    failed |= Protocol_CRC32_Check("分段计算",
                                   split_crc,
                                   PROTOCOL_CRC32_TEST_EXPECTED);
    failed |= Protocol_CRC32_Check("空数据计算",
                                   empty_crc,
                                   PROTOCOL_CRC32_EMPTY_EXPECTED);
    failed |= Protocol_CRC32_Check("空指针保护",
                                   null_crc,
                                   PROTOCOL_CRC32_EMPTY_EXPECTED);

    if (failed != 0)
    {
        return 1;
    }

    (void)printf("CRC32 测试通过：123456789=0x%08lX，空数据=0x%08lX\n",
                 (unsigned long)calculate_crc,
                 (unsigned long)empty_crc);
    return 0;
}
