#include "camera_pc_dump.h"      // 引入 PC Dump 模块自己的头文件，包含尺寸、命令返回值和函数声明
#include "camera_cli.h"          // 引入 CLI 模块，用于处理 HELP、STATUS、PROC、THR、RESET 等文本命令
#include "camera_frame_buffer.h" // 引入双缓冲模块，用于获得 DCMI 采集缓冲区和待发送的前台帧
#include "protocol_crc32.h"      // 引入公共 CRC32 模块，用于计算图像 payload 的 CRC32 校验值

#include <string.h>              // 提供 memset()，用于清空文本命令行缓冲区

//============================================================================
// @file    camera_pc_dump.c
// @brief   OV5640 图像帧 UART 导出模块
//          本模块同时承担两个功能：
//          1. 接收逐字节输入的文本命令，并识别 DUMP 或交给 camera_cli 处理。
//          2. 将前台 RGB565 图像封装为 OV56RGB5 数据帧，通过 UART 发送到 PC。
//============================================================================

// UART 每次发送的最大数据块为 1024 字节，避免一次 HAL_UART_Transmit() 传入过大的长度
#define PC_DUMP_UART_CHUNK_SIZE  1024U

// 文本命令缓冲区总长度为 32 字节，其中最后 1 字节必须预留给字符串结束符 '\0'
#define PC_DUMP_COMMAND_LINE_LEN 32U

// 保存当前正在接收的一行文本命令，例如 "STATUS"、"PROC GRAY" 或 "DUMP"
static char s_camera_pc_dump_line[PC_DUMP_COMMAND_LINE_LEN];

// 记录当前命令缓冲区中已经保存了多少个有效字符，不包括末尾的 '\0'
static uint8_t s_camera_pc_dump_line_length;

// 表示当前这一行是否已经超过缓冲区容量，1 表示已经溢出，之后的字符会一直丢弃到换行符
static uint8_t s_camera_pc_dump_line_overflow;

// 将一个 ASCII 小写字母转换成大写字母，方便后续实现命令不区分大小写
static char Camera_PC_Dump_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))               // 只处理 ASCII 范围内的 a～z
    {
        return (char)(ch - ('a' - 'A'));           // 小写字母减去 32，得到对应的大写字母
    }

    return ch;                                     // 非小写字母保持原值，例如数字、空格和大写字母
}

// 判断一个字符是不是文本命令中的空白字符
static uint8_t Camera_PC_Dump_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U; // 空格或制表符返回 1，否则返回 0
}

// 跳过字符串左侧所有空格和制表符，返回第一个非空白字符的位置
static const char *Camera_PC_Dump_TrimLeft(const char *line)
{
    while ((line != NULL) &&                       // 先确保传入的字符串指针有效
           (Camera_PC_Dump_IsSpace(*line) != 0U)) // 当前字符是空格或制表符时继续向后移动
    {
        ++line;                                    // 指针移动到下一个字符，不实际修改原字符串内容
    }

    return line;                                   // 返回去除左侧空白后的字符串起始地址
}

// 计算字符串去除右侧空格和制表符后的有效长度
static uint32_t Camera_PC_Dump_TrimmedLength(const char *line)
{
    uint32_t len = 0U;                             // len 最终表示去掉右侧空白后的有效字符数

    if (line == NULL)                              // 空指针没有可计算的字符串内容
    {
        return 0U;                                 // 返回长度 0，避免访问非法地址
    }

    while (line[len] != '\0')                      // 从字符串开头寻找末尾的 '\0'
    {
        ++len;                                     // 每发现一个普通字符，字符串长度加 1
    }

    while ((len > 0U) &&                           // 字符串至少还有一个字符时才检查末尾
           (Camera_PC_Dump_IsSpace(line[len - 1U]) != 0U)) // 最后一个字符是空格或制表符
    {
        --len;                                     // 去掉一个右侧空白字符，继续检查新的末尾
    }

    return len;                                    // 返回去除右侧空白后的实际有效长度
}

// 判断一整行文本是否与指定命令完全相等，比较时忽略大小写和首尾空白
static uint8_t Camera_PC_Dump_LineEquals(const char *line, const char *token)
{
    const char *trimmed = Camera_PC_Dump_TrimLeft(line); // 先跳过命令行左侧的空格和制表符
    uint32_t len = Camera_PC_Dump_TrimmedLength(trimmed); // 再得到去除右侧空白后的有效长度
    uint32_t i = 0U;                               // i 用于逐字符比较 token 和命令行

    while (token[i] != '\0')                       // token 未到字符串末尾时继续比较
    {
        if (i >= len)                              // 命令行已经结束，但 token 还有字符
        {
            return 0U;                             // 说明命令行长度不足，不可能完全匹配
        }

        if (Camera_PC_Dump_ToUpper(trimmed[i]) != token[i]) // 命令行字符转成大写后再比较
        {
            return 0U;                             // 任意一个字符不同，立即判定命令不匹配
        }

        ++i;                                       // 当前字符匹配，继续比较下一个字符
    }

    return (i == len) ? 1U : 0U;                  // token结束后，命令行也必须同时结束才算完全匹配
}

// 清空一行文本命令的缓存、长度和溢出状态
static void Camera_PC_Dump_ResetLine(char *line,
                                     uint32_t line_size,
                                     uint8_t *line_length,
                                     uint8_t *line_overflow)
{
    if ((line != NULL) && (line_size > 0U))        // 缓冲区指针有效且大小不为 0
    {
        (void)memset(line, 0, line_size);          // 将整个字符数组清零，避免旧命令残留
    }

    if (line_length != NULL)                       // 调用者提供了长度变量地址
    {
        *line_length = 0U;                         // 当前命令行重新从 0 个字符开始
    }

    if (line_overflow != NULL)                     // 调用者提供了溢出标志变量地址
    {
        *line_overflow = 0U;                       // 清除命令行溢出状态
    }
}

// 将一个 16 位整数按照小端序写入 2 字节数组
static void Camera_PC_Dump_WriteU16LE(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);             // 第 0 字节保存 value 的低 8 位
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);      // 第 1 字节保存 value 的高 8 位
}

// 将一个 32 位整数按照小端序写入 4 字节数组
static void Camera_PC_Dump_WriteU32LE(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);             // 第 0 字节保存最低 8 位
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);      // 第 1 字节保存第 8～15 位
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);     // 第 2 字节保存第 16～23 位
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);     // 第 3 字节保存最高 8 位
}

// 获取 DCMI DMA 当前应该写入的图像缓冲区地址
uint32_t Camera_PC_Dump_GetBufferAddress(void)
{
    // DCMI DMA 必须写入 back buffer，不能直接写正在被 UART 发送的 front buffer
    return (uint32_t)Camera_FrameBuffer_GetBackBuffer();
}

// 获取 DCMI DMA 需要传输的 32 位数据数量
uint32_t Camera_PC_Dump_GetWordCount(void)
{
    // DCMI DMA 的长度单位通常是 32 位 word，而不是字节，所以返回预先计算好的 word 数
    return PC_DUMP_WORD_COUNT;
}

// 无任何串口输出地复位文本命令解析器
void Camera_PC_Dump_ResetCommandParser(void)
{
    // UART 错误恢复或 StreamBuffer 溢出后，需要放弃当前接收到一半的旧命令
    Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                             sizeof(s_camera_pc_dump_line),
                             &s_camera_pc_dump_line_length,
                             &s_camera_pc_dump_line_overflow);
}

// 将一个 UART 接收字节送入文本命令行解析器
uint8_t Camera_PC_Dump_FeedCommandByte(UART_HandleTypeDef *huart, uint8_t byte)
{
    if (huart == NULL)                             // CLI 回复需要使用 UART 句柄，因此必须先检查
    {
        return CAMERA_PC_DUMP_CMD_NONE;            // UART 句柄无效，不能继续解析和输出响应
    }

    // 收到回车 '\r' 或换行 '\n' 时，认为当前文本行已经结束
    if ((byte == '\r') || (byte == '\n'))
    {
        // 如果此前命令长度超过 31 字节，则整条命令视为无效命令
        if (s_camera_pc_dump_line_overflow != 0U)
        {
            // 将固定字符串 "UNKNOWN" 交给 CLI，使 CLI 按未知命令路径输出一次错误提示
            (void)Camera_CLI_HandleLine(huart, "UNKNOWN");

            // 当前超长命令已经到达行结束符，可以恢复正常命令接收状态
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);

            return CAMERA_PC_DUMP_CMD_CLI;         // 表示已经完成了一次 CLI 命令处理
        }

        // 当前行没有保存任何有效字符，说明收到的是空行或 CRLF 中剩余的第二个换行字符
        if (s_camera_pc_dump_line_length == 0U)
        {
            // 再次复位一次，确保长度、缓冲区和溢出标志处于干净状态
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);

            // 空行不属于有效命令，也不需要向 PC 输出 unknown command
            return CAMERA_PC_DUMP_CMD_PENDING;
        }

        // C字符串必须以 '\0' 结尾，CLI 模块后续才能按字符串处理当前命令
        s_camera_pc_dump_line[s_camera_pc_dump_line_length] = '\0';

        // DUMP 命令必须在本模块中单独识别，不能直接交给普通 CLI 输出文字
        if (Camera_PC_Dump_LineEquals(s_camera_pc_dump_line, "DUMP") != 0U)
        {
            // 识别完成后立即清空当前命令，为下一条命令做准备
            Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                     sizeof(s_camera_pc_dump_line),
                                     &s_camera_pc_dump_line_length,
                                     &s_camera_pc_dump_line_overflow);

            // 只返回 DUMP 事件，不在这里发送图像，也不输出任何文本
            return CAMERA_PC_DUMP_CMD_DUMP;
        }

        // HELP、STATUS、PROC、THR、RESET 等其他命令由 camera_cli 模块解释和执行
        (void)Camera_CLI_HandleLine(huart, s_camera_pc_dump_line);

        // 当前命令已经处理完成，清空缓存，开始等待下一条命令
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 &s_camera_pc_dump_line_overflow);

        return CAMERA_PC_DUMP_CMD_CLI;             // 表示一个普通 CLI 命令已经处理完成
    }

    // 小于 0x20 的字符通常属于控制字符，但制表符 '\t' 被保留用于命令中的空白
    if (((byte < 0x20U) && (byte != '\t')) ||      // 过滤 NUL、ESC 等不可打印控制字符
        (byte == 0x7FU))                           // 同时过滤 ASCII DEL 字符
    {
        return CAMERA_PC_DUMP_CMD_PENDING;         // 丢弃无效字符，继续等待后续命令字节
    }

    // 如果当前命令已经溢出，后面的所有普通字符都继续丢弃，直到收到 '\r' 或 '\n'
    if (s_camera_pc_dump_line_overflow != 0U)
    {
        return CAMERA_PC_DUMP_CMD_PENDING;         // 不再往缓冲区写入，避免数组越界
    }

    // 缓冲区最后一个位置必须留给字符串结束符 '\0'，所以最多保存 31 个普通字符
    if (s_camera_pc_dump_line_length <
        (uint8_t)(sizeof(s_camera_pc_dump_line) - 1U))
    {
        // 将当前有效字符保存到命令行数组末尾，再将已保存长度增加 1
        s_camera_pc_dump_line[s_camera_pc_dump_line_length++] = (char)byte;
    }
    else
    {
        // 当前字符无法再写入数组，说明这条命令已经超过允许的最大长度
        Camera_PC_Dump_ResetLine(s_camera_pc_dump_line,
                                 sizeof(s_camera_pc_dump_line),
                                 &s_camera_pc_dump_line_length,
                                 NULL);             // 这里暂时不清溢出标志，因为下一行会主动设置

        // 标记当前整行已经溢出，后续字节会一直丢弃到换行符
        s_camera_pc_dump_line_overflow = 1U;
    }

    // 当前还没有遇到行结束符，命令尚未完整，需要继续接收后续字节
    return CAMERA_PC_DUMP_CMD_PENDING;
}

// 将前台图像帧封装为 OV56RGB5 格式，并通过 UART 阻塞发送到 PC
uint8_t Camera_PC_Dump_SendFrame(UART_HandleTypeDef *huart, uint32_t frame_id)
{
    // magic 是图像帧固定标识，PC端会搜索这8字节判断图像帧从哪里开始
    static const uint8_t magic[8] =
    {
        'O', 'V', '5', '6', 'R', 'G', 'B', '5'
    };

    uint8_t header[22];                             // 保存完整22字节OV56RGB5帧头
    uint8_t crc_bytes[4];                           // 保存按照小端序排列的4字节CRC32
    CameraFrame_t frame;                            // 保存前台帧的数据地址、宽、高和字节数
    const uint8_t *payload;                         // 指向真正要发送的38400字节RGB565图像
    uint32_t offset = 0U;                           // 记录payload已经发送到哪个字节位置
    uint32_t crc;                                   // 保存对完整payload计算出的CRC32结果

    if (huart == NULL)                              // UART句柄为空时无法发送任何数据
    {
        return 1U;                                  // 错误码1表示传入参数无效
    }

    // 从双缓冲模块取得当前稳定的front buffer，而不是仍可能被DCMI修改的back buffer
    if ((Camera_FrameBuffer_GetFrontFrame(&frame) != CAMERA_FB_OK) ||
        (frame.data == NULL) ||                     // 图像数据指针不能为空
        (frame.width != PC_DUMP_WIDTH) ||           // 图像宽度必须等于协议固定宽度160
        (frame.height != PC_DUMP_HEIGHT) ||         // 图像高度必须等于协议固定高度120
        (frame.size_bytes != PC_DUMP_PAYLOAD_LEN))  // RGB565数据长度必须等于160×120×2
    {
        return 5U;                                  // 错误码5表示前台图像帧无效或尺寸不匹配
    }

    // 将void类型的帧数据地址转换成只读字节指针，便于逐字节计算CRC和发送
    payload = (const uint8_t *)frame.data;

    // 将固定8字节magic复制到header的0～7字节
    for (uint32_t i = 0U; i < sizeof(magic); ++i)
    {
        header[i] = magic[i];                       // 逐字节复制，最终得到ASCII字符串OV56RGB5
    }

    header[8] = 1U;                                 // 协议版本号，目前固定为版本1
    header[9] = 1U;                                 // 图像格式编号，目前1表示RGB565
    Camera_PC_Dump_WriteU16LE(&header[10],
                              (uint16_t)PC_DUMP_WIDTH); // header[10～11]保存宽度160
    Camera_PC_Dump_WriteU16LE(&header[12],
                              (uint16_t)PC_DUMP_HEIGHT); // header[12～13]保存高度120
    Camera_PC_Dump_WriteU32LE(&header[14],
                              PC_DUMP_PAYLOAD_LEN);     // header[14～17]保存payload长度38400
    Camera_PC_Dump_WriteU32LE(&header[18],
                              frame_id);                // header[18～21]保存STM32图像帧编号

    // 只对38400字节图像payload计算CRC，当前协议不把22字节header包含在CRC范围内
    crc = Protocol_CRC32_Calculate(payload, PC_DUMP_PAYLOAD_LEN);

    // 将32位CRC转换为4字节小端序，随后直接通过UART发送
    Camera_PC_Dump_WriteU32LE(crc_bytes, crc);

    // 先发送22字节header，PC端必须先解析header才能知道payload长度和frame_id
    if (HAL_UART_Transmit(huart,
                          header,
                          sizeof(header),
                          HAL_MAX_DELAY) != HAL_OK)
    {
        return 2U;                                  // 错误码2表示header发送失败
    }

    // payload总长度为38400字节，循环分成若干个不超过1024字节的数据块
    while (offset < PC_DUMP_PAYLOAD_LEN)
    {
        // 计算当前还剩多少字节没有发送
        uint32_t remaining = PC_DUMP_PAYLOAD_LEN - offset;

        // 如果剩余数据大于1024，就发送1024；否则发送最后不足1024的部分
        uint16_t chunk =
            (uint16_t)((remaining > PC_DUMP_UART_CHUNK_SIZE)
                       ? PC_DUMP_UART_CHUNK_SIZE
                       : remaining);

        // 从payload[offset]开始，阻塞发送当前chunk字节数据
        if (HAL_UART_Transmit(huart,
                              (uint8_t *)&payload[offset],
                              chunk,
                              HAL_MAX_DELAY) != HAL_OK)
        {
            return 3U;                              // 错误码3表示图像payload中途发送失败
        }

        offset += chunk;                            // 更新已发送位置，下一次从新的offset继续发送
    }

    // payload全部发送完成后，再发送4字节CRC32作为整帧结尾
    if (HAL_UART_Transmit(huart,
                          crc_bytes,
                          sizeof(crc_bytes),
                          HAL_MAX_DELAY) != HAL_OK)
    {
        return 4U;                                  // 错误码4表示CRC字段发送失败
    }

    return 0U;                                      // 返回0表示header、payload和CRC全部发送成功
}