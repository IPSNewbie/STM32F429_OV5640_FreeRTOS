#ifndef PROTOCOL_CRC32_H
#define PROTOCOL_CRC32_H

#include <stddef.h>  // 提供数据块长度使用的 size_t
#include <stdint.h>  // 提供 CRC 状态和输入字节使用的固定宽度整数类型

/**
 * @file protocol_crc32.h
 * @brief DUMP 与 binary image request 共用的 CRC32 计算接口
 *
 * 算法参数为 initial=0xFFFFFFFF、反射多项式 0xEDB88320、final xor=0xFFFFFFFF。
 * DUMP 调用方覆盖完整 RGB565 payload；请求解析器覆盖 version/type/seq/length。
 * 本模块不决定覆盖范围，也不负责把 uint32_t 按小端写入 UART 字节流。
 * @note CRC32 用于发现传输损坏，不承担加密、身份认证或防篡改作用。
 */

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
 * @param data 输入数据；按当前容错语义，为 NULL 时无论 length 为何均返回原状态
 * @param length 输入数据长度；0 是合法空输入，不会访问 data
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
 * @param data 输入数据；为 NULL 时按空输入处理，即使 length 非零也不会报错
 * @param length 输入数据长度
 * @return 最终 CRC32 结果；空输入返回 Init 状态经 Finalize 后的标准结果
 */
uint32_t Protocol_CRC32_Calculate(const uint8_t *data, size_t length);

#endif
