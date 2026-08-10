#include "protocol_crc32.h"  // CRC32 增量状态和一次性计算的公开接口

//============================================================================
// @file    protocol_crc32.c
// @brief   UART 图像协议共用的反射 CRC32 计算模块
//
// DUMP/OV56RGB5 使用本算法覆盖完整 RGB565 payload，不包含 header；binary image
// request 则只覆盖 version、type、sequence、payload length 六个业务字段字节。
// 覆盖范围和最终结果的小端序列化由调用模块决定，本模块只计算 CRC 数值。
// 算法采用 initial=0xFFFFFFFF、反射多项式 0xEDB88320、final xor=0xFFFFFFFF。
// 模块没有全局可变状态，可由不同调用方各自保存 crc 状态进行分块计算。
// CRC32 只能发现常见传输误码、错位或截断，不能提供身份认证或防恶意篡改。
//============================================================================

// 计算开始前的全 1 内部状态；此时尚未执行最终异或。
#define PROTOCOL_CRC32_INITIAL_VALUE        0xFFFFFFFFU
// 标准 CRC-32 多项式的 LSB-first 反射形式，配合逐位右移实现。
#define PROTOCOL_CRC32_REFLECTED_POLYNOMIAL 0xEDB88320U
// 所有输入字节累计完成后再异或全 1，得到可发送或比较的最终值。
#define PROTOCOL_CRC32_FINAL_XOR_VALUE      0xFFFFFFFFU

// 返回一条新 CRC 计算链的初始内部状态，后续必须先 Update 再 Finalize。
uint32_t Protocol_CRC32_Init(void)
{
    return PROTOCOL_CRC32_INITIAL_VALUE;
}

// 将一个字节同步累加到已有状态；局部 data 的地址只在被调函数返回前使用。
uint32_t Protocol_CRC32_UpdateByte(uint32_t crc, uint8_t data)
{
    return Protocol_CRC32_Update(crc, &data, 1U);
}

// 按协议约定增量处理一段数据，使大 payload 可以按 UART 分块边读边累计。
// 外层循环恰好消费 length 个字节，内层每字节固定处理 8 bit；两层都有确定上界，
// 不等待硬件、也不涉及 timeout。data 为 NULL 时按当前实现保持原状态。
uint32_t Protocol_CRC32_Update(uint32_t crc,
                               const uint8_t *data,
                               size_t length)
{
    size_t i;   // 当前处理到输入块的第几个字节
    uint32_t bit;  // 当前字节固定执行的 8 次 LSB-first 更新计数

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
            // 0-(crc&1) 生成全 0 或全 1 mask：最低位为 1 时才异或反射多项式。
            crc = (crc >> 1) ^
                  ((0U - (crc & 1U)) & PROTOCOL_CRC32_REFLECTED_POLYNOMIAL);
        }
    }

    return crc;
}

// 对尚未最终化的内部状态执行一次最终异或；结果不能再继续传给 Update。
uint32_t Protocol_CRC32_Finalize(uint32_t crc)
{
    return crc ^ PROTOCOL_CRC32_FINAL_XOR_VALUE;
}

// 按 Init→Update→Finalize 三步一次性计算完整数据块，适合已在内存中的 payload。
uint32_t Protocol_CRC32_Calculate(const uint8_t *data, size_t length)
{
    uint32_t crc = Protocol_CRC32_Init();

    crc = Protocol_CRC32_Update(crc, data, length);
    return Protocol_CRC32_Finalize(crc);
}
