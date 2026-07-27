# UART 二进制图像请求解析器日志

## 阶段

Stage 9 Round 2：纯 C 二进制图像请求协议状态机

## 当前基础

1. Stage 9 Round 1 已完成并提交。
2. CRC32 公共模块已完成，标准向量 `123456789` 的结果为 `0xCBF43926`。
3. OV56RGB5 图像响应协议保持不变，完整帧仍为 38426 B。
4. 当前 UART RX 仍使用 `HAL_UART_Receive()` 单字节轮询。
5. 本轮解析器未接入 UART、CameraServiceTask、CLI 或 DUMP。
6. 项目串口背景为 COM4、115200、8N1、无流控，但本轮没有进行 COM4 协议实测。
7. 本轮未配置 DMA、IDLE 或 StreamBuffer。

## 请求帧格式

PC 到 STM32 的 v1 请求帧固定为 14 B：

| 偏移 | 字段 | 长度 | 内容 |
|---:|---|---:|---|
| 0 | SOF0 | 1 B | `0xA5` |
| 1 | SOF1 | 1 B | `0x5A` |
| 2 | version | 1 B | `0x01` |
| 3 | msg_type | 1 B | `0x20`，REQUEST_IMAGE |
| 4 | seq low | 1 B | 序号低字节 |
| 5 | seq high | 1 B | 序号高字节 |
| 6 | payload_len low | 1 B | v1 固定为 `0x00` |
| 7 | payload_len high | 1 B | v1 固定为 `0x00` |
| 8 | crc32 byte 0 | 1 B | CRC最低字节 |
| 9 | crc32 byte 1 | 1 B | CRC字节1 |
| 10 | crc32 byte 2 | 1 B | CRC字节2 |
| 11 | crc32 byte 3 | 1 B | CRC最高字节 |
| 12 | EOF0 | 1 B | `0x0D` |
| 13 | EOF1 | 1 B | `0x0A` |

多字节字段均为小端序。v1 不支持载荷，`payload_len` 必须为 0。

CRC32 沿用 Round 1 的 CRC-32/ISO-HDLC 公共模块，只覆盖以下 6 B：

```text
version
msg_type
seq low
seq high
payload_len low
payload_len high
```

CRC 不包含 SOF、CRC 字段自身和 EOF。

固定向量：

```text
seq = 0x1234
CRC 输入 = 01 20 34 12 00 00
CRC32 = 0xDBB445EA
完整帧 = A5 5A 01 20 34 12 00 00 EA 45 B4 DB 0D 0A
```

主机测试中的构造函数通过 `Protocol_CRC32_Calculate()` 实际计算 CRC，并确认生成的 14 B 与固定向量逐字节一致。

## 状态机

解析器包含以下 14 个状态：

```text
SYNC0
  ↓ A5
SYNC1
  ↓ 5A
VERSION
  ↓
MSG_TYPE
  ↓
SEQ_LOW
  ↓
SEQ_HIGH
  ↓
LEN_LOW
  ↓
LEN_HIGH
  ↓
CRC0
  ↓
CRC1
  ↓
CRC2
  ↓
CRC3
  ↓
EOF0
  ↓
EOF1
  ↓
OK / ERROR / RESYNC
```

主要转换规则：

1. `SYNC0` 忽略随机垃圾字节，仅在收到 `0xA5` 后进入 `SYNC1`。
2. `SYNC1` 收到 `0x5A` 后进入 VERSION 并初始化 CRC。
3. `SYNC1` 再次收到 `0xA5` 时，把后一个 `0xA5` 作为新的 SOF0。
4. version 必须为 `0x01`，msg_type 必须为 `0x20`。
5. seq 的低、高字节依次加入 CRC，并按小端组成 `uint16_t`。
6. payload_len 的低、高字节依次加入 CRC，收到高字节后才校验是否为 0。
7. 4 B received CRC 按小端拼接，不加入 computed CRC。
8. EOF0 和 EOF1 均正确后才最终化并比较 CRC。
9. 成功时先把内部 frame 复制到独立的 `out_frame`，再自动复位解析器。

错误优先级按最先发现的位置确定：

1. version 错误返回 VERSION_ERROR。
2. msg_type 错误返回 TYPE_ERROR。
3. payload_len 非 0 返回 LENGTH_ERROR。
4. EOF0 或 EOF1 错误返回 EOF_ERROR。
5. EOF 完整正确但 CRC 不匹配返回 CRC_ERROR。

当 CRC 与 EOF 同时错误时先返回 EOF_ERROR，因为完整帧边界尚未确认。

## 解析结果

| 结果 | 含义 |
|---|---|
| `IMAGE_REQUEST_PARSE_NONE` | 尚未进入有效候选帧 |
| `IMAGE_REQUEST_PARSE_PENDING` | 候选帧尚未接收完整 |
| `IMAGE_REQUEST_PARSE_OK` | 合法请求帧 |
| `IMAGE_REQUEST_PARSE_CRC_ERROR` | CRC不匹配 |
| `IMAGE_REQUEST_PARSE_VERSION_ERROR` | 版本不支持 |
| `IMAGE_REQUEST_PARSE_TYPE_ERROR` | 消息类型不支持 |
| `IMAGE_REQUEST_PARSE_LENGTH_ERROR` | payload_len不是0 |
| `IMAGE_REQUEST_PARSE_EOF_ERROR` | 帧尾错误 |
| `IMAGE_REQUEST_PARSE_TIMEOUT` | 半帧超时 |
| `IMAGE_REQUEST_PARSE_BAD_ARGUMENT` | 参数为空 |

## 重同步规则

解析错误后统一清除上一候选帧的 frame、computed CRC、received CRC、时间和活动标志，再根据当前错误字节处理：

```text
当前字节 == 0xA5：
    state = SYNC1
    frame_active = 1
    last_byte_time_ms = now_ms
    当前 A5 作为下一候选帧的新 SOF0

当前字节 != 0xA5：
    state = SYNC0
    frame_active = 0
```

重同步不覆盖当前实际错误返回值。例如 version 字节为 `0xA5` 时，本次仍返回 VERSION_ERROR，但解析器会保留该字节并在下一次输入 `0x5A` 后继续新帧。

同步阶段的普通失败不作为协议错误：

```text
A5 5A    -> 找到帧头
A5 A5    -> 后一个 A5 成为新 SOF0
A5 其他  -> 返回 NONE 并回到 SYNC0
```

主机测试已覆盖随机垃圾前缀、`A5 A5 5A`、连续多个 `A5`、完整错误帧后紧接合法帧，以及错误字节本身为 `0xA5` 的快速重同步。

## 超时规则

半帧超时固定为 100 ms。只有 `frame_active != 0` 时才检查：

```c
(uint32_t)(now_ms - parser->last_byte_time_ms) >=
    IMAGE_REQUEST_TIMEOUT_MS
```

使用无符号减法可以正确处理 `uint32_t` tick 回绕。

`ImageRequestProtocol_CheckTimeout()` 的行为：

| 条件 | 返回值 |
|---|---|
| parser为空 | BAD_ARGUMENT |
| 当前空闲 | NONE |
| 活动且小于100 ms | PENDING |
| 活动且达到100 ms | 复位并返回TIMEOUT |

`ImageRequestProtocol_FeedByte()` 在处理新字节前先检查旧候选帧是否超时。若已超时，本次返回 TIMEOUT，当前新字节只用于重同步，不再作为旧帧字段处理。当前新字节为 `0xA5` 时会保留为新的 SOF0。

主机测试已覆盖99 ms、100 ms、超过100 ms、逐字节间隔小于100 ms、超时新字节是否为 `0xA5`、超时后合法帧，以及 tick 回绕前后的边界。

## 初始化与输出约束

1. Init 和 Reset 都把 state、frame、两个CRC、时间和活动标志清零。
2. Init 和 Reset 传入空指针时安全返回。
3. FeedByte 的 parser 或 out_frame 为空时返回 BAD_ARGUMENT，不消耗字节，也不改变有效 parser 状态。
4. out_frame 必须是独立对象，不能指向 parser 内部 frame。
5. 只有 OK 才写 out_frame；NONE、PENDING、所有错误、TIMEOUT 和 BAD_ARGUMENT 都保持原值。
6. OK 后解析器自动回到 SYNC0，可立即接收下一帧。
7. `ImageRequestProtocol_IsActive(NULL)` 返回0。
8. 普通成功、错误和 `CheckTimeout()` 超时后活动标志为0。
9. 错误或 FeedByte 超时的当前字节为 `0xA5` 时，由于已保留新SOF0，活动标志为1。

## 模块边界

解析器模块：

1. 只依赖标准固定宽度整数类型和 `protocol_crc32` 公共模块。
2. 不依赖 HAL、FreeRTOS、CMSIS、CameraServiceTask 或 CLI。
3. 不读取系统时钟，时间由调用者传入。
4. 不输出日志。
5. 不进行动态内存分配。
6. 不保存完整14 B帧，也不分配payload缓冲区。
7. 可由调用者静态分配并逐字节输入。
8. 当前未连接真实UART，也不会触发图像采集或DUMP。

## 主机测试

测试使用 Visual Studio C11 编译器，选项为：

```text
/std:c11 /W4 /WX /utf-8
```

测试直接链接：

```text
tests/image_request_protocol_test.c
BSPDrivers/Src/image_request_protocol.c
BSPDrivers/Src/protocol_crc32.c
```

测试矩阵：

| 类别 | 数量 | 结果 |
|---|---:|---|
| 基本合法帧 | 8 | 全部通过 |
| 帧头和重同步 | 6 | 全部通过 |
| 字段错误 | 12 | 全部通过 |
| 超时 | 10 | 全部通过 |
| 参数和状态安全 | 11 | 全部通过 |
| 合计 | 47 | 通过47，失败0 |

实际输出：

```text
固定帧 CRC32=0xDBB445EA，测试总数=47，通过=47，失败=0
```

测试进程退出码为0。固定帧、seq `0x0000`、`0x1234`、`0xFFFF`、连续帧、错误恢复、CRC和EOF错误优先级、100 ms超时、tick回绕、空指针及非OK不写输出均已在主机侧验证。

## 构建结果

1. `cmake --build build/Debug`：通过。
2. `image_request_protocol.c`：已由现有递归CMake规则进入STM32固件编译。
3. `tests/image_request_protocol_test.c`：未进入STM32固件目标。
4. RAM：115592 B / 192 KB，58.79%。
5. CCMRAM：0 B / 64 KB，0.00%。
6. FLASH：54376 B / 1 MB，5.19%。
7. 解析器尚未被业务代码引用，未引用函数段被链接器移除，因此当前链接占用与Round 1一致。
8. HAL、FreeRTOS、CMSIS和系统时钟依赖扫描：无匹配。
9. 动态内存调用扫描：无匹配。
10. DMA、IDLE和StreamBuffer新增实现扫描：无匹配。
11. 旧 `HAL_UART_Receive()` 单字节轮询仍存在且未修改。
12. `git diff --check`：通过，仅有工作区换行格式提示，无空白错误。

## 风险与限制

1. v1 只支持 `payload_len == 0`，不解析或跳过非零载荷。
2. 当前解析器尚未接入真实UART，因此没有COM4板上协议实测结果。
3. 当前没有ACK/NACK。
4. 当前没有重传。
5. 当前没有二进制响应帧。
6. 当前没有文本/二进制协议分发器。
7. 当前没有UART DMA、IDLE或StreamBuffer。
8. STM32图像响应仍计划复用OV56RGB5，但Round 2不会触发该响应。
9. Round 3迁移DMA接收链路、Round 4接入协议分发后仍需进行硬件验证。
10. `uint32_t`时间差算法适合100 ms超时；若活动候选帧超过完整32位tick周期且从未检查，单次时间戳无法区分多次回绕。

## 阶段结论

Stage 9 Round 2 的纯 C 二进制图像请求解析器已完成主机侧功能验证：47项测试全部通过，固定CRC为 `0xDBB445EA`，主机测试退出码为0，STM32固件编译链接通过。模块保持独立，未修改或接入现有UART、CameraServiceTask、DUMP、OV56RGB5或Python工具。真实UART集成与COM4板上验证不属于本轮完成范围。
