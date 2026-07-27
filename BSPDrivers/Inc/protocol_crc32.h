#ifndef PROTOCOL_CRC32_H
#define PROTOCOL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 获取尚未执行最终异或的 CRC32 初始状态
 * @return CRC32 初始状态
 */
uint32_t Protocol_CRC32_Init(void);

/**
 * @brief 使用一个字节更新尚未最终化的 CRC32 状态
 * @param crc 由初始化或上一次更新得到的内部状态
 * @param data 输入字节
 * @return 更新后的内部状态
 */
uint32_t Protocol_CRC32_UpdateByte(uint32_t crc, uint8_t data);

/**
 * @brief 分块更新尚未最终化的 CRC32 状态
 * @param crc 由初始化或上一次更新得到的内部状态
 * @param data 输入数据；为空时不处理任何字节并返回原状态
 * @param length 输入数据长度；大于零时调用方仍应保证 data 有效
 * @return 更新后的内部状态
 * @note 正确顺序为初始化、一次或多次更新、最终化；最终化后的值不能继续更新
 */
uint32_t Protocol_CRC32_Update(uint32_t crc,
                               const uint8_t *data,
                               size_t length);

/**
 * @brief 对内部状态执行最终异或
 * @param crc 尚未最终化的 CRC32 内部状态
 * @return 可用于比较或发送的 CRC32 结果
 */
uint32_t Protocol_CRC32_Finalize(uint32_t crc);

/**
 * @brief 一次性计算数据的 CRC32
 * @param data 输入数据；为空时按空数据处理
 * @param length 输入数据长度
 * @return 最终 CRC32 结果
 */
uint32_t Protocol_CRC32_Calculate(const uint8_t *data, size_t length);

#endif
