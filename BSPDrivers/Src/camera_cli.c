#include "camera_cli.h"          // 引入 CLI 模块自身的声明，例如 Camera_CLI_Init()、Camera_CLI_HandleLine() 和相关枚举类型
#include "camera_sd_storage.h"   // 引入 SD 卡软件状态和受控初始化请求接口，本阶段不操作 SDIO 硬件
#include "camera_snapshot_control.h" // 引入拍照保存前后的相机控制边界软件接口
#include "camera_frame_buffer.h" // 引入帧缓冲区尺寸宏，例如 CAMERA_FB_WIDTH 和 CAMERA_FB_HEIGHT
#include "camera_rtos.h"         // 引入 RTOS 运行统计接口，用于记录 CLI 命令次数并读取 CameraRtosStats_t
#include "uart_rx_dma.h"         // 引入 UART DMA 接收统计接口，用于 STATUS 命令输出 DMA 和 StreamBuffer 状态

#include <stddef.h>              // 提供 NULL、size_t 等标准定义

#define CAMERA_CLI_DEFAULT_THRESHOLD 128U // 定义二值化默认阈值；灰度值大于等于 128 时通常输出白色，否则输出黑色

static CameraCliRuntimeConfig_t s_camera_cli_config; // 保存 CLI 当前运行配置；由于使用 static，该变量只允许本文件访问，并且整个程序运行期间一直存在

static void Camera_CLI_WriteText(UART_HandleTypeDef *huart, const char *text) // 通过指定 UART 发送一段以 '\0' 结束的字符串，不自动添加换行符
{
    const char *p = text; // 定义字符指针 p，从字符串首地址开始移动，用于手动计算字符串长度
    uint16_t len = 0U;    // 保存字符串有效字符数量；不包含最后的字符串结束符 '\0'

    if ((huart == NULL) || (text == NULL)) // 检查 UART 句柄和字符串指针是否有效，避免访问空指针
    {
        return; // 参数无效时立即结束函数，不执行 UART 发送
    }

    while (*p != '\0') // 从字符串首字符开始遍历，直到遇到 C 字符串结束符 '\0'
    {
        ++p;   // 指针移动到下一个字符
        ++len; // 每经过一个有效字符，字符串长度加 1
    }

    if (len > 0U) // 只有字符串中确实存在有效字符时才调用 UART 发送，避免发送 0 字节数据
    {
        (void)HAL_UART_Transmit(huart,              // 调用 STM32 HAL 阻塞式 UART 发送函数
                                (uint8_t *)text,    // HAL 接口要求 uint8_t 指针，因此将 const char * 转换为 uint8_t *
                                len,                // 指定要发送的有效字符数量，不包含 '\0'
                                HAL_MAX_DELAY);     // 一直等待直到发送完成；因此该函数会阻塞当前 CameraServiceTask
    }
}

static char Camera_CLI_ToUpper(char ch) // 将一个小写英文字母转换成对应大写字母，用于命令不区分大小写匹配
{
    if ((ch >= 'a') && (ch <= 'z')) // 只有字符处于小写字母 a～z 范围时才需要转换
    {
        return (char)(ch - ('a' - 'A')); // 利用 ASCII 大小写字母之间固定差值，将小写字符转换为大写字符
    }

    return ch; // 非小写字母直接原样返回，例如数字、空格、下划线和本身已经是大写的字母
}

static uint8_t Camera_CLI_IsSpace(char ch) // 判断一个字符是否属于本 CLI 支持的空白字符
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U; // 普通空格或制表符返回 1，其他字符返回 0；不把 '\r'、'\n' 视为参数空格
}

static const char *Camera_CLI_TrimLeft(const char *line) // 跳过字符串左侧连续的空格和制表符，返回第一个非空白字符的位置
{
    while ((line != NULL) &&                         // 首先保证当前字符串指针不是 NULL
           (Camera_CLI_IsSpace(*line) != 0U))        // 当前字符仍然是空格或制表符时继续移动
    {
        ++line; // 指针向后移动一个字符，相当于忽略一个左侧空白字符
    }

    return line; // 返回去除左侧空白后的起始地址；原始字符串内容没有被修改
}

static uint32_t Camera_CLI_TrimmedLength(const char *line) // 计算字符串去除右侧空格和制表符后的有效长度，不修改原字符串
{
    uint32_t len = 0U; // 保存字符串当前长度，先计算完整长度，再从末尾向前删除空白

    if (line == NULL) // 防止通过空指针访问字符串
    {
        return 0U; // NULL 字符串按有效长度 0 处理
    }

    while (line[len] != '\0') // 从索引 0 开始向后寻找字符串结束符
    {
        ++len; // 每找到一个有效字符，完整字符串长度加 1
    }

    while ((len > 0U) &&                                    // 长度必须大于 0，防止访问 line[-1]
           (Camera_CLI_IsSpace(line[len - 1U]) != 0U))       // 检查当前最后一个字符是否为空格或制表符
    {
        --len; // 如果末尾是空白字符，则将有效长度减 1，相当于从逻辑上删除右侧空白
    }

    return len; // 返回去除右侧空白后的有效长度；字符串中的字符没有被真正改写
}

static uint8_t Camera_CLI_TokenEquals(const char *text, // 指向待比较的命令或参数字符串
                                      uint32_t len,     // text 中参与比较的有效字符数量
                                      const char *token) // 指向预定义的大写命令令牌，例如 "HELP" 或 "BYPASS"
{
    uint32_t i = 0U; // 当前比较到的字符索引

    while (token[i] != '\0') // 遍历预定义 token 中的每个字符，直到 token 的结束符
    {
        if (i >= len) // 如果 text 已经结束，但 token 还有字符，说明 text 比 token 短
        {
            return 0U; // 字符串长度不一致，判定不匹配
        }

        if (Camera_CLI_ToUpper(text[i]) != token[i]) // 将用户输入字符转成大写，再与预定义的大写 token 字符比较
        {
            return 0U; // 任意字符不同，立即判定两个令牌不匹配
        }

        ++i; // 当前字符相同，继续比较下一个字符
    }

    return (i == len) ? 1U : 0U; // token 比较结束后，只有 text 长度也恰好结束才算完全匹配，避免 "HELPXXX" 被识别为 "HELP"
}

static const char *Camera_CLI_ModeName(CameraProcessMode_t mode) // 将图像处理模式枚举值转换为便于串口显示的字符串
{
    if (mode == CAMERA_PROCESS_MODE_BYPASS) // 判断是否为旁路模式，即不对图像做灰度或二值化处理
    {
        return "BYPASS"; // 返回旁路模式名称
    }

    if (mode == CAMERA_PROCESS_MODE_GRAYSCALE) // 判断是否为灰度图处理模式
    {
        return "GRAYSCALE"; // 返回灰度模式名称
    }

    if (mode == CAMERA_PROCESS_MODE_BINARY) // 判断是否为二值化处理模式
    {
        return "BINARY"; // 返回二值化模式名称
    }

    return "UNKNOWN"; // 遇到未定义或损坏的枚举值时返回 UNKNOWN，便于排查异常
}

static void Camera_CLI_WriteU32(UART_HandleTypeDef *huart, uint32_t value) // 将 uint32_t 十进制数转换成文本并通过 UART 发送
{
    char buf[11];              // uint32_t 最大值 4294967295 共 10 位，再加 1 字节 '\0'，因此需要 11 字节
    uint32_t pos = sizeof(buf); // pos 初始指向数组末尾后一位，后续从后向前填充数字字符

    buf[--pos] = '\0'; // 先在数组最后一个位置写入字符串结束符，使后面生成的内容成为合法 C 字符串

    do // 至少执行一次，确保 value 为 0 时也能正确输出字符 "0"
    {
        buf[--pos] = (char)('0' + (value % 10U)); // 取 value 的个位数字，并转换成对应 ASCII 字符写入缓冲区
        value /= 10U;                             // 删除已经处理的个位，继续处理更高位
    } while ((value != 0U) &&                     // 只要还有更高位数字就继续转换
             (pos > 0U));                         // 同时确保不会越过数组起始地址

    Camera_CLI_WriteText(huart, &buf[pos]); // 从当前第一个有效数字字符开始发送，前面未使用的数组空间不会被发送
}

static void Camera_CLI_WriteLine(UART_HandleTypeDef *huart, const char *text) // 发送一行文本，并自动在末尾追加标准串口换行 "\r\n"
{
    Camera_CLI_WriteText(huart, text);   // 先发送调用者提供的正文内容
    Camera_CLI_WriteText(huart, "\r\n"); // 再发送回车加换行，使终端显示到下一行
}

static void Camera_CLI_WriteStatLine(UART_HandleTypeDef *huart, // 要输出统计信息的 UART 句柄
                                     const char *name,          // 统计字段名称，例如 dump_success_count
                                     uint32_t value)            // 该统计字段对应的 32 位无符号整数值
{
    Camera_CLI_WriteText(huart, "  ");  // 输出两个空格，用于让统计字段在 RTOS 或 UART RX DMA 标题下缩进显示
    Camera_CLI_WriteText(huart, name);  // 输出统计字段名称
    Camera_CLI_WriteText(huart, "=");   // 输出字段名和值之间的等号
    Camera_CLI_WriteU32(huart, value);  // 将数值转换为十进制文本并发送
    Camera_CLI_WriteText(huart, "\r\n"); // 输出一行结束符，使下一个统计字段显示在新行
}

static uint8_t Camera_CLI_ParseU8(const char *text, // 指向待解析的数字字符串，例如 "128"
                                  uint32_t len,     // 待解析字符串的有效字符长度
                                  uint8_t *value)   // 用于返回解析后的 0～255 数值
{
    uint32_t parsed = 0U; // 使用 uint32_t 暂存中间结果，便于在转换过程中检查是否超过 255

    if ((text == NULL) ||  // 检查输入字符串是否有效
        (value == NULL) || // 检查输出指针是否有效
        (len == 0U))       // 空字符串不能解析成合法阈值
    {
        return 0U; // 参数无效或没有数字字符，返回解析失败
    }

    for (uint32_t i = 0U; i < len; ++i) // 逐个检查并转换字符串中的每一个字符
    {
        if ((text[i] < '0') || (text[i] > '9')) // 当前字符不是十进制数字 0～9
        {
            return 0U; // 出现字母、负号、小数点等非数字字符时立即返回失败
        }

        parsed = (parsed * 10U) +                  // 原有数值整体乘 10，为新的一位十进制数字腾出个位
                 (uint32_t)(text[i] - '0');        // 将 ASCII 数字字符转换为实际数字并累加

        if (parsed > 255U) // 二值化阈值使用 uint8_t，只允许 0～255
        {
            return 0U; // 一旦中间结果超过 255，立即返回失败，避免最终强制转换发生截断
        }
    }

    *value = (uint8_t)parsed; // 所有字符均合法且结果不超过 255，将结果写入调用者提供的变量
    return 1U;                // 返回 1 表示解析成功
}

void Camera_CLI_ResetDefault(void) // 将运行时 CLI 配置恢复为项目定义的默认值
{
    s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;          // 默认使用 BYPASS，保证上电后输出原始彩色图像
    s_camera_cli_config.binary_threshold = CAMERA_CLI_DEFAULT_THRESHOLD;     // 默认二值化阈值设置为 128
}

void Camera_CLI_Init(void) // 初始化 CLI 模块；当前只需要初始化运行时配置
{
    Camera_CLI_ResetDefault(); // 复用默认重置函数，避免在多个位置重复写默认参数
    Camera_SDStorage_InitState(); // 初始化 SD 卡模块的软件状态，不访问 SDIO、GPIO 或 FATFS
    Camera_SnapshotControl_InitState(); // 初始化相机控制边界软件状态，不操作 DCMI、DMA 或 GPIO
}

CameraProcessMode_t Camera_CLI_GetProcessMode(void) // 获取当前 CLI 选择的图像处理模式
{
    return s_camera_cli_config.process_mode; // 返回 BYPASS、GRAYSCALE 或 BINARY 中的当前值
}

uint8_t Camera_CLI_GetBinaryThreshold(void) // 获取当前二值化阈值
{
    return s_camera_cli_config.binary_threshold; // 返回 0～255 范围内的阈值，供图像处理模块使用
}

const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void) // 获取整个 CLI 运行时配置结构体的只读指针
{
    return &s_camera_cli_config; // 返回静态配置变量地址；const 限制调用者不能通过该指针修改配置
}

static void Camera_CLI_PrintHelp(UART_HandleTypeDef *huart) // 输出当前 CLI 支持的全部文本命令
{
    Camera_CLI_WriteLine(huart, "HELP");         // HELP：显示当前帮助列表
    Camera_CLI_WriteLine(huart, "STATUS");       // STATUS：显示图像参数、RTOS统计和UART DMA统计
    Camera_CLI_WriteLine(huart, "PROC");         // PROC：查询当前图像处理模式
    Camera_CLI_WriteLine(huart, "PROC BYPASS");  // PROC BYPASS：切换为原始彩色图像旁路输出
    Camera_CLI_WriteLine(huart, "PROC GRAY");    // PROC GRAY：切换为灰度图输出
    Camera_CLI_WriteLine(huart, "PROC BINARY");  // PROC BINARY：切换为二值图输出
    Camera_CLI_WriteLine(huart, "THR");          // THR：查询当前二值化阈值
    Camera_CLI_WriteLine(huart, "THR 0..255");   // THR 数值：将二值化阈值修改为 0～255
    Camera_CLI_WriteLine(huart, "RESET");        // RESET：恢复 BYPASS 模式和默认阈值 128
    Camera_CLI_WriteLine(huart, "DUMP");         // DUMP：触发一次图像采集并发送 OV56RGB5 二进制帧
    Camera_CLI_WriteLine(huart, "SD STATUS - show SD storage status"); // 查询 SD 卡模块的软件状态
    Camera_CLI_WriteLine(huart, "SD INIT - request SD card init, currently deferred until SDIO takeover"); // 请求初始化，本阶段延后到 SDIO 接管完成后
    Camera_CLI_WriteLine(huart, "SD TAKEOVER STATUS - show SDIO takeover status"); // 查询 SDIO 接管软件状态
    Camera_CLI_WriteLine(huart, "SD TAKEOVER ENTER - request SDIO takeover, currently deferred"); // 请求进入接管模式，本阶段只记录请求
    Camera_CLI_WriteLine(huart, "SD TAKEOVER EXIT - request leaving SDIO takeover, currently deferred"); // 请求退出接管模式，本阶段只记录请求
    Camera_CLI_WriteLine(huart, "SNAPSHOT STATUS - show snapshot camera control status"); // 查询相机控制边界软件状态
    Camera_CLI_WriteLine(huart, "SNAPSHOT PREPARE - stop DCMI before SD save and activate software guard"); // 停止 DCMI 并激活软件保护
    Camera_CLI_WriteLine(huart, "SNAPSHOT RESTORE - request camera restore boundary after SD save, currently deferred"); // 请求恢复相机采集边界
    Camera_CLI_WriteLine(huart, "IWDGTEST CAMERA_TIMEOUT - simulate camera heartbeat timeout and wait for IWDG reset"); // IWDG故障路径测试
}

static void Camera_CLI_PrintStatus(UART_HandleTypeDef *huart) // 输出当前图像配置、RTOS运行统计、健康状态和UART DMA统计
{
    const CameraRtosStats_t *stats;       // 保存 CameraServiceTask、MonitorTask、DUMP和协议统计结构体指针
    const UartRxDmaStats_t *uart_dma_stats; // 保存 UART DMA 接收事件、字节数、溢出和恢复统计结构体指针

    Camera_RTOS_RecordStatus(HAL_GetTick()); // 记录本次执行 STATUS 命令的系统时间，供 last_status_time_ms 字段显示
    stats = Camera_RTOS_GetStats();          // 获取 RTOS 运行统计结构体的只读指针
    uart_dma_stats = UART_RxDma_GetStats();  // 获取 UART RX DMA 统计结构体的只读指针

    Camera_CLI_WriteText(huart, "process mode: ");                             // 输出处理模式字段名称
    Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode)); // 将当前模式转换为字符串后输出并换行

    Camera_CLI_WriteText(huart, "binary threshold: ");                         // 输出二值化阈值字段名称
    Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);          // 将当前阈值转换为十进制文本输出
    Camera_CLI_WriteText(huart, "\r\n");                                       // 输出换行结束当前字段

    Camera_CLI_WriteLine(huart, "AEC: OV5640_AEC_TARGET_BASELINE");            // 输出当前固定使用的 AEC 基准模式
    Camera_CLI_WriteLine(huart, "AWB: OV5640_AWB_MODE_AUTO");                  // 输出当前固定使用的自动白平衡模式
    Camera_CLI_WriteLine(huart, "image tuning: brightness=+1 contrast=0 saturation=1 sharpness=0"); // 输出当前固定图像调参配置

    Camera_CLI_WriteText(huart, "frame size: ");                               // 输出图像尺寸字段名称
    Camera_CLI_WriteU32(huart, CAMERA_FB_WIDTH);                               // 输出帧缓冲区宽度，当前为 160
    Camera_CLI_WriteText(huart, "x");                                          // 输出宽度和高度之间的分隔符
    Camera_CLI_WriteU32(huart, CAMERA_FB_HEIGHT);                              // 输出帧缓冲区高度，当前为 120
    Camera_CLI_WriteText(huart, "\r\n");                                       // 输出换行结束图像尺寸字段

    if (stats == NULL) // 检查 RTOS 统计指针是否有效
    {
        return; // 如果无法获取 RTOS 统计，只保留前面已经输出的基础图像配置，然后结束函数
    }

    Camera_CLI_WriteLine(huart, "RTOS:"); // 输出 RTOS 统计分组标题

    Camera_CLI_WriteStatLine(huart,                                   // 输出 CameraServiceTask 主循环运行次数
                             "camera_service_loop_count",              // 字段名
                             stats->camera_service_loop_count);        // CameraServiceTask 每完成一轮主循环时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出 MonitorTask 心跳次数
                             "monitor_tick_count",                     // 字段名
                             stats->monitor_tick_count);               // MonitorTask 通常每秒增加一次

    Camera_CLI_WriteStatLine(huart,                                   // 输出系统运行时间统计
                             "uptime_ms",                              // 字段名
                             stats->uptime_ms);                        // MonitorTask 按 1000 ms 累计的运行时间

    Camera_CLI_WriteStatLine(huart,                                   // 输出已接收并处理的有效 CLI 命令总数
                             "cli_command_count",                      // 字段名
                             stats->cli_command_count);                // 每次非空文本命令进入 CLI 主入口时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出未知 CLI 命令次数
                             "cli_unknown_count",                      // 字段名
                             stats->cli_unknown_count);                // 无法匹配 HELP、STATUS、PROC、THR、RESET 时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出所有图像发送请求总数
                             "dump_request_count",                     // 字段名
                             stats->dump_request_count);               // 文本 DUMP 和二进制 REQUEST_IMAGE 都进入同一计数

    Camera_CLI_WriteStatLine(huart,                                   // 输出成功发送图像帧的次数
                             "dump_success_count",                     // 字段名
                             stats->dump_success_count);               // 完成采集、处理及 OV56RGB5 发送后增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出图像发送失败次数
                             "dump_error_count",                       // 字段名
                             stats->dump_error_count);                 // 采集、处理或发送任一步失败时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出兼容保留的 UART 无数据状态次数
                             "uart_none_count",                        // 字段名
                             stats->uart_none_count);                  // 早期轮询架构遗留统计，DMA架构下主要用于兼容观察

    Camera_CLI_WriteStatLine(huart,                                   // 输出兼容保留的 UART 部分命令状态次数
                             "uart_pending_count",                     // 字段名
                             stats->uart_pending_count);               // 表示曾经收到部分文本或协议数据但尚未形成完整命令

    Camera_CLI_WriteStatLine(huart,                                   // 输出上层识别的 UART 错误次数
                             "uart_error_count",                       // 字段名
                             stats->uart_error_count);                 // 与底层 uart_dma_error_count 含义不同，属于 CameraServiceTask 统计

    Camera_CLI_WriteStatLine(huart,                                   // 输出成功解析的合法二进制图像请求数量
                             "binary_request_count",                   // 字段名
                             stats->binary_request_count);             // 14 B 请求通过版本、类型、长度、CRC和帧尾检查后增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出最终成功返回图像的二进制请求数量
                             "binary_request_success_count",           // 字段名
                             stats->binary_request_success_count);     // 合法二进制请求对应的 OV56RGB5 成功发送后增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出二进制请求解析错误总数
                             "binary_request_error_count",             // 字段名
                             stats->binary_request_error_count);       // version、type、length、CRC、EOF 错误都会计入

    Camera_CLI_WriteStatLine(huart,                                   // 输出请求 CRC 校验失败次数
                             "binary_request_crc_error_count",         // 字段名
                             stats->binary_request_crc_error_count);   // 收到完整帧但 CRC 与本地计算结果不一致时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出不支持协议版本的请求次数
                             "binary_request_version_error_count",     // 字段名
                             stats->binary_request_version_error_count); // version 不等于 0x01 时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出不支持消息类型的请求次数
                             "binary_request_type_error_count",        // 字段名
                             stats->binary_request_type_error_count);  // msg_type 不等于 REQUEST_IMAGE 0x20 时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出请求载荷长度非法次数
                             "binary_request_length_error_count",      // 字段名
                             stats->binary_request_length_error_count); // v1 中 payload_len 不等于 0 时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出请求帧尾错误次数
                             "binary_request_eof_error_count",         // 字段名
                             stats->binary_request_eof_error_count);   // 帧尾不是 0x0D 0x0A 时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出二进制候选帧接收超时次数
                             "binary_request_timeout_count",           // 字段名
                             stats->binary_request_timeout_count);     // 半帧超过 100 ms 未完成时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次合法二进制请求的 seq
                             "last_binary_request_seq",                // 字段名
                             stats->last_binary_request_seq);          // seq 是 PC 请求编号，与 OV56RGB5 frame_id 相互独立

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次二进制解析错误枚举值
                             "last_binary_error_code",                 // 字段名
                             stats->last_binary_error_code);           // 用数值表示最后发生的是 CRC、VERSION、TYPE 等哪种错误

    Camera_CLI_WriteStatLine(huart,                                   // 输出整个 CameraServiceTask 最近一次错误码
                             "last_error_code",                        // 字段名
                             stats->last_error_code);                  // 包括 CLI、DUMP、UART 或其他上层业务错误

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次图像请求的处理总耗时
                             "last_dump_time_ms",                      // 字段名
                             stats->last_dump_time_ms);                // 当前115200波特率下通常约为3.4～3.5秒

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次 STATUS 命令执行时的 HAL tick
                             "last_status_time_ms",                    // 字段名
                             stats->last_status_time_ms);              // 调用 Camera_RTOS_RecordStatus(HAL_GetTick()) 时更新

    Camera_CLI_WriteLine(huart, "HEALTH:"); // 输出系统健康监控分组标题

    Camera_CLI_WriteStatLine(huart,                                   // 输出健康数据采样次数
                             "health_sample_count",                    // 字段名
                             stats->health_sample_count);              // 每完成一次任务栈和堆余量采样时增加

    Camera_CLI_WriteStatLine(huart,                                   // 输出 CameraServiceTask 历史最小剩余栈空间
                             "camera_service_stack_min_free_bytes",    // 字段名
                             stats->camera_service_stack_min_free_bytes); // 值越小表示任务越接近栈溢出

    Camera_CLI_WriteStatLine(huart,                                   // 输出 MonitorTask 历史最小剩余栈空间
                             "monitor_stack_min_free_bytes",           // 字段名
                             stats->monitor_stack_min_free_bytes);     // 用于判断 MonitorTask 分配的栈是否充足

    Camera_CLI_WriteStatLine(huart,                                   // 输出当前 FreeRTOS heap 剩余字节数
                             "free_heap_bytes",                        // 字段名
                             stats->free_heap_bytes);                  // 表示当前动态内存池还剩多少可用空间

    Camera_CLI_WriteStatLine(huart,                                   // 输出系统运行以来 heap 的历史最低剩余量
                             "min_ever_free_heap_bytes",               // 字段名
                             stats->min_ever_free_heap_bytes);         // 用于判断最坏情况下动态内存是否接近耗尽

    Camera_CLI_WriteLine(huart, "HOOK:"); // 输出FreeRTOS严重错误Hook状态分组标题

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次Hook故障类型
                             "hook_fault_code",                       // 字段名
                             stats->hook_fault_code);                  // 0正常，1栈溢出，2内存分配失败，3断言失败

    Camera_CLI_WriteStatLine(huart,                                   // 输出Hook累计触发次数
                             "hook_fault_count",                      // 字段名
                             stats->hook_fault_count);                 // 正常运行时为0

    Camera_CLI_WriteStatLine(huart,                                   // 输出最近一次configASSERT失败行号
                             "assert_line",                           // 字段名
                             stats->assert_line);                      // 非断言故障或正常运行时为0

    Camera_CLI_WriteLine(huart, "HEARTBEAT:"); // 输出核心任务心跳状态分组标题

    Camera_CLI_WriteStatLine(huart,
                             "camera_service_heartbeat_count",
                             stats->camera_service_heartbeat_count);
    Camera_CLI_WriteStatLine(huart,
                             "monitor_heartbeat_count",
                             stats->monitor_heartbeat_count);
    Camera_CLI_WriteStatLine(huart,
                             "camera_service_heartbeat_ms",
                             stats->camera_service_heartbeat_ms);
    Camera_CLI_WriteStatLine(huart,
                             "monitor_heartbeat_ms",
                             stats->monitor_heartbeat_ms);
    Camera_CLI_WriteStatLine(huart,
                             "camera_service_heartbeat_age_ms",
                             stats->camera_service_heartbeat_age_ms);
    Camera_CLI_WriteStatLine(huart,
                             "monitor_heartbeat_age_ms",
                             stats->monitor_heartbeat_age_ms);

    Camera_CLI_WriteLine(huart, "IWDG:"); // 输出独立看门狗运行状态分组标题

    Camera_CLI_WriteStatLine(huart, "iwdg_enabled", stats->iwdg_enabled);
    Camera_CLI_WriteStatLine(huart, "iwdg_refresh_count", stats->iwdg_refresh_count);
    Camera_CLI_WriteStatLine(huart,
                             "iwdg_refresh_skip_count",
                             stats->iwdg_refresh_skip_count);
    Camera_CLI_WriteStatLine(huart,
                             "iwdg_last_refresh_ms",
                             stats->iwdg_last_refresh_ms);
    Camera_CLI_WriteStatLine(huart, "iwdg_last_skip_ms", stats->iwdg_last_skip_ms);
    Camera_CLI_WriteStatLine(huart,
                             "iwdg_last_skip_reason",
                             stats->iwdg_last_skip_reason);
    Camera_CLI_WriteStatLine(huart, "iwdg_timeout_ms", stats->iwdg_timeout_ms);
    Camera_CLI_WriteStatLine(huart,
                             "iwdg_camera_age_limit_ms",
                             stats->iwdg_camera_age_limit_ms);
    Camera_CLI_WriteStatLine(huart,
                             "iwdg_monitor_age_limit_ms",
                             stats->iwdg_monitor_age_limit_ms);
    Camera_CLI_WriteStatLine(huart, "iwdg_test_mode", stats->iwdg_test_mode);

    if (uart_dma_stats != NULL) // 只有成功获取 UART DMA 统计指针时才输出下面的 DMA 接收统计
    {
        Camera_CLI_WriteLine(huart, "UART RX DMA:"); // 输出 UART DMA 接收统计分组标题

        Camera_CLI_WriteStatLine(huart,                               // 输出 UART DMA 接收事件回调总次数
                                 "uart_dma_event_count",               // 字段名
                                 uart_dma_stats->rx_event_count);      // HT、TC或IDLE产生有效接收事件时增加

        Camera_CLI_WriteStatLine(huart,                               // 输出 DMA 接收到的UART总字节数
                                 "uart_dma_rx_bytes",                  // 字段名
                                 uart_dma_stats->rx_bytes);            // ISR计算本次新增数据长度后累计

        Camera_CLI_WriteStatLine(huart,                               // 输出成功写入StreamBuffer的字节总数
                                 "stream_buffer_write_bytes",          // 字段名
                                 uart_dma_stats->stream_write_bytes);  // 正常情况下应与uart_dma_rx_bytes相同

        Camera_CLI_WriteStatLine(huart,                               // 输出因StreamBuffer空间不足而丢弃的字节总数
                                 "stream_buffer_overflow_bytes",       // 字段名
                                 uart_dma_stats->stream_overflow_bytes); // 大于0表示任务未及时读取，接收数据曾发生溢出

        Camera_CLI_WriteStatLine(huart,                               // 输出HAL UART错误回调触发次数
                                 "uart_dma_error_count",               // 字段名
                                 uart_dma_stats->uart_error_count);    // 包括ORE、FE、NE、PE等接收错误

        Camera_CLI_WriteStatLine(huart,                               // 输出UART DMA接收链路成功恢复次数
                                 "uart_dma_recovery_count",            // 字段名
                                 uart_dma_stats->recovery_count);      // CameraServiceTask完成Abort、清错和重启DMA后增加

        Camera_CLI_WriteStatLine(huart,                               // 输出StreamBuffer溢出后的协议重同步次数
                                 "stream_buffer_resync_count",         // 字段名
                                 uart_dma_stats->stream_resync_count); // 排空残留数据并复位文本和二进制解析器时增加
    }
}

static CameraCliStatus_t Camera_CLI_HandleProc(UART_HandleTypeDef *huart, // UART句柄，用于输出查询结果或设置结果
                                               const char *arg,          // PROC命令后面的参数起始地址
                                               uint32_t arg_len)         // 参数去除首尾空白后的有效长度
{
    if (arg_len == 0U) // PROC 后没有参数，表示只查询当前模式，不修改配置
    {
        Camera_CLI_WriteText(huart, "process mode: ");                             // 输出字段说明
        Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode)); // 输出当前模式名称并换行
        return CAMERA_CLI_OK;                                                      // 查询成功
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "BYPASS") != 0U) // 参数是否等于 BYPASS，不区分用户输入大小写
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS; // 设置为原始彩色图像旁路模式
    }
    else if ((Camera_CLI_TokenEquals(arg, arg_len, "GRAY") != 0U) ||      // 支持较短写法 GRAY
             (Camera_CLI_TokenEquals(arg, arg_len, "GRAYSCALE") != 0U))   // 同时支持完整写法 GRAYSCALE
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_GRAYSCALE; // 设置为灰度图处理模式
    }
    else if (Camera_CLI_TokenEquals(arg, arg_len, "BINARY") != 0U) // 参数是否为 BINARY
    {
        s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BINARY; // 设置为二值图处理模式
    }
    else // 参数既不是 BYPASS、GRAY、GRAYSCALE，也不是 BINARY
    {
        Camera_CLI_WriteLine(huart, "ERR bad PROC arg"); // 通过串口提示 PROC 参数非法
        return CAMERA_CLI_ERROR_BAD_ARG;                 // 返回参数错误状态
    }

    Camera_CLI_WriteText(huart, "OK process mode: ");                             // 输出设置成功提示
    Camera_CLI_WriteLine(huart, Camera_CLI_ModeName(s_camera_cli_config.process_mode)); // 输出修改后的实际模式名称
    return CAMERA_CLI_OK;                                                        // 返回处理成功
}

static CameraCliStatus_t Camera_CLI_HandleThreshold(UART_HandleTypeDef *huart, // UART句柄，用于输出阈值或错误信息
                                                    const char *arg,          // THR命令后的参数字符串
                                                    uint32_t arg_len)         // 参数有效长度
{
    uint8_t threshold; // 保存解析成功后的0～255阈值

    if (arg_len == 0U) // THR 后没有参数，表示只查询当前阈值
    {
        Camera_CLI_WriteText(huart, "binary threshold: ");                  // 输出字段名称
        Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);   // 输出当前阈值
        Camera_CLI_WriteText(huart, "\r\n");                                // 输出换行
        return CAMERA_CLI_OK;                                               // 查询成功
    }

    if (Camera_CLI_ParseU8(arg, arg_len, &threshold) == 0U) // 尝试将参数解析为0～255的十进制整数
    {
        Camera_CLI_WriteLine(huart, "ERR bad THR arg"); // 参数包含非数字或超出255时输出错误
        return CAMERA_CLI_ERROR_BAD_ARG;                // 返回参数错误
    }

    s_camera_cli_config.binary_threshold = threshold;                  // 将解析成功的数值保存为新的二值化阈值
    Camera_CLI_WriteText(huart, "OK binary threshold: ");              // 输出设置成功提示
    Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);  // 输出实际保存的新阈值
    Camera_CLI_WriteText(huart, "\r\n");                               // 输出换行
    return CAMERA_CLI_OK;                                              // 返回处理成功
}

/* 输出 SDIO 接管状态字段，供 SD STATUS 和 SD TAKEOVER STATUS 复用。 */
static void Camera_CLI_PrintSdTakeoverFields(
    UART_HandleTypeDef *huart,
    const CameraSdStorageStatus_t *status)
{
    Camera_CLI_WriteStatLine(huart, "takeover_state", status->takeover_state);
    Camera_CLI_WriteText(huart, "  takeover_state_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SDStorage_TakeoverStateToString(status->takeover_state));
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_enter_attempt_count",
        status->takeover_enter_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_exit_attempt_count",
        status->takeover_exit_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_enter_success_count",
        status->takeover_enter_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_exit_success_count",
        status->takeover_exit_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_error_count",
        status->takeover_error_count);
    Camera_CLI_WriteStatLine(
        huart,
        "last_takeover_error_code",
        status->last_takeover_error_code);
    Camera_CLI_WriteText(huart, "  last_takeover_error_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SDStorage_ErrorToString(status->last_takeover_error_code));
    Camera_CLI_WriteStatLine(
        huart,
        "last_takeover_operation_ms",
        status->last_takeover_operation_ms);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_precheck_required",
        status->takeover_precheck_required);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_precheck_attempt_count",
        status->takeover_precheck_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_precheck_success_count",
        status->takeover_precheck_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "takeover_precheck_fail_count",
        status->takeover_precheck_fail_count);
    Camera_CLI_WriteStatLine(
        huart,
        "snapshot_pause_required",
        status->snapshot_pause_required);
    Camera_CLI_WriteStatLine(
        huart,
        "snapshot_pause_confirmed",
        status->snapshot_pause_confirmed);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_release_ready",
        status->conflict_pin_release_ready);
    Camera_CLI_WriteStatLine(
        huart,
        "last_takeover_precheck_error_code",
        status->last_takeover_precheck_error_code);
    Camera_CLI_WriteText(huart, "  last_takeover_precheck_error_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SDStorage_ErrorToString(
            status->last_takeover_precheck_error_code));
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_release_attempt_count",
        status->conflict_pin_release_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_release_success_count",
        status->conflict_pin_release_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_release_error_count",
        status->conflict_pin_release_error_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_restore_attempt_count",
        status->conflict_pin_restore_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_restore_success_count",
        status->conflict_pin_restore_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_restore_error_count",
        status->conflict_pin_restore_error_count);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pins_released",
        status->conflict_pins_released);
    Camera_CLI_WriteStatLine(
        huart,
        "last_conflict_pin_error_code",
        status->last_conflict_pin_error_code);
    Camera_CLI_WriteText(huart, "  last_conflict_pin_error_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SDStorage_ErrorToString(status->last_conflict_pin_error_code));
    Camera_CLI_WriteStatLine(
        huart,
        "last_conflict_pin_operation_ms",
        status->last_conflict_pin_operation_ms);
}

/* 输出完整 SD 卡软件状态，不访问 SDIO 或 FATFS。 */
static void Camera_CLI_PrintSdStatus(UART_HandleTypeDef *huart)
{
    CameraSdStorageStatus_t status;

    Camera_SDStorage_GetStatus(&status);

    Camera_CLI_WriteLine(huart, "SD:");
    Camera_CLI_WriteStatLine(huart, "is_initialized", status.is_initialized);
    Camera_CLI_WriteStatLine(huart, "takeover_required", status.takeover_required);
    Camera_CLI_WriteStatLine(huart, "sdio_ready", status.sdio_ready);
    Camera_CLI_WriteStatLine(huart, "fatfs_ready", status.fatfs_ready);
    Camera_CLI_WriteStatLine(huart, "init_attempt_count", status.init_attempt_count);
    Camera_CLI_WriteStatLine(huart, "init_success_count", status.init_success_count);
    Camera_CLI_WriteStatLine(huart, "init_error_count", status.init_error_count);
    Camera_CLI_WriteStatLine(huart, "last_error_code", status.last_error_code);
    Camera_CLI_WriteText(huart, "  last_error_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SDStorage_ErrorToString(status.last_error_code));
    Camera_CLI_WriteStatLine(huart, "last_operation_ms", status.last_operation_ms);
    Camera_CLI_PrintSdTakeoverFields(huart, &status);
}

/* 单独输出 SDIO 接管状态，不执行任何硬件接管操作。 */
static void Camera_CLI_PrintSdTakeoverStatus(UART_HandleTypeDef *huart)
{
    CameraSdStorageStatus_t status;

    Camera_SDStorage_GetStatus(&status);
    Camera_CLI_WriteLine(huart, "SD TAKEOVER:");
    Camera_CLI_PrintSdTakeoverFields(huart, &status);
}

/* 处理 SD 命令；初始化和接管请求当前都只记录软件状态。 */
static CameraCliStatus_t Camera_CLI_HandleSd(UART_HandleTypeDef *huart,
                                             const char *arg,
                                             uint32_t arg_len)
{
    uint32_t result;

    if (Camera_CLI_TokenEquals(arg, arg_len, "STATUS") != 0U)
    {
        Camera_CLI_PrintSdStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "INIT") != 0U)
    {
        result = Camera_SDStorage_RequestInit();

        if (result == CAMERA_SD_ERR_NEED_TAKEOVER)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.");
        }
        else
        {
            Camera_CLI_WriteText(huart, "SD INIT: ");
            Camera_CLI_WriteLine(huart, Camera_SDStorage_ErrorToString(result));
        }

        Camera_CLI_PrintSdStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "TAKEOVER STATUS") != 0U)
    {
        Camera_CLI_PrintSdTakeoverStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "TAKEOVER ENTER") != 0U)
    {
        result = Camera_SDStorage_RequestTakeoverEnter();

        if (result == CAMERA_SD_ERR_SNAPSHOT_NOT_PAUSED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.");
        }
        else if (result == CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD TAKEOVER ENTER: conflict pins released, GPIO switch to SDIO is not implemented yet.");
        }
        else if (result == CAMERA_SD_ERR_CONFLICT_PIN_RELEASE_FAILED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD TAKEOVER ENTER: conflict pin release failed.");
        }
        else
        {
            Camera_CLI_WriteText(huart, "SD TAKEOVER ENTER: ");
            Camera_CLI_WriteLine(huart, Camera_SDStorage_ErrorToString(result));
        }

        Camera_CLI_PrintSdTakeoverStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "TAKEOVER EXIT") != 0U)
    {
        result = Camera_SDStorage_RequestTakeoverExit();

        if (result == CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD TAKEOVER EXIT: conflict pins restored to DCMI AF13, SDIO restore is not implemented yet.");
        }
        else if (result == CAMERA_SD_ERR_CONFLICT_PIN_RESTORE_FAILED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SD TAKEOVER EXIT: conflict pin restore failed.");
        }
        else
        {
            Camera_CLI_WriteText(huart, "SD TAKEOVER EXIT: ");
            Camera_CLI_WriteLine(huart, Camera_SDStorage_ErrorToString(result));
        }

        Camera_CLI_PrintSdTakeoverStatus(huart);
        return CAMERA_CLI_OK;
    }

    Camera_CLI_WriteLine(huart, "ERR bad SD arg");
    return CAMERA_CLI_ERROR_BAD_ARG;
}

/* 输出拍照保存前后的相机控制边界状态，不访问任何硬件。 */
static void Camera_CLI_PrintSnapshotStatus(UART_HandleTypeDef *huart)
{
    CameraSnapshotControlStatus_t status;

    Camera_SnapshotControl_GetStatus(&status);

    Camera_CLI_WriteLine(huart, "SNAPSHOT:");
    Camera_CLI_WriteStatLine(
        huart,
        "camera_control_state",
        status.camera_control_state);
    Camera_CLI_WriteText(huart, "  camera_control_state_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SnapshotControl_StateToString(status.camera_control_state));
    Camera_CLI_WriteStatLine(
        huart,
        "prepare_attempt_count",
        status.prepare_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "restore_attempt_count",
        status.restore_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "prepare_success_count",
        status.prepare_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "restore_success_count",
        status.restore_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "control_error_count",
        status.control_error_count);
    Camera_CLI_WriteStatLine(huart, "last_error_code", status.last_error_code);
    Camera_CLI_WriteText(huart, "  last_error_text=");
    Camera_CLI_WriteLine(
        huart,
        Camera_SnapshotControl_ErrorToString(status.last_error_code));
    Camera_CLI_WriteStatLine(
        huart,
        "last_operation_ms",
        status.last_operation_ms);
    Camera_CLI_WriteStatLine(
        huart,
        "real_dcmi_stop_enabled",
        status.real_dcmi_stop_enabled);
    Camera_CLI_WriteStatLine(
        huart,
        "dcmi_stop_attempt_count",
        status.dcmi_stop_attempt_count);
    Camera_CLI_WriteStatLine(
        huart,
        "dcmi_stop_success_count",
        status.dcmi_stop_success_count);
    Camera_CLI_WriteStatLine(
        huart,
        "dcmi_stop_error_count",
        status.dcmi_stop_error_count);
    Camera_CLI_WriteStatLine(
        huart,
        "last_dcmi_stop_hal_status",
        status.last_dcmi_stop_hal_status);
    Camera_CLI_WriteStatLine(
        huart,
        "dcmi_stop_required",
        status.dcmi_stop_required);
    Camera_CLI_WriteStatLine(
        huart,
        "dcmi_dma_stop_required",
        status.dcmi_dma_stop_required);
    Camera_CLI_WriteStatLine(
        huart,
        "conflict_pin_release_required",
        status.conflict_pin_release_required);
    Camera_CLI_WriteStatLine(
        huart,
        "camera_restore_required",
        status.camera_restore_required);
    Camera_CLI_WriteStatLine(
        huart,
        "frame_buffer_required",
        status.frame_buffer_required);
    Camera_CLI_WriteStatLine(
        huart,
        "frame_buffer_ready",
        status.frame_buffer_ready);
    Camera_CLI_WriteStatLine(
        huart,
        "software_guard_active",
        status.software_guard_active);
    Camera_CLI_WriteStatLine(
        huart,
        "dump_block_required",
        status.dump_block_required);
    Camera_CLI_WriteStatLine(
        huart,
        "dump_block_count",
        status.dump_block_count);
    Camera_CLI_WriteStatLine(
        huart,
        "binary_block_count",
        status.binary_block_count);
}

/* 处理 SNAPSHOT 控制边界命令；PREPARE 停止 DCMI，RESTORE 仍不重启 DCMI。 */
static CameraCliStatus_t Camera_CLI_HandleSnapshot(UART_HandleTypeDef *huart,
                                                   const char *arg,
                                                   uint32_t arg_len)
{
    uint32_t result;

    if (Camera_CLI_TokenEquals(arg, arg_len, "STATUS") != 0U)
    {
        Camera_CLI_PrintSnapshotStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "PREPARE") != 0U)
    {
        result = Camera_SnapshotControl_RequestPrepare();

        if (result == CAMERA_SNAPSHOT_OK)
        {
            Camera_CLI_WriteLine(
                huart,
                "SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.");
        }
        else if (result == CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SNAPSHOT PREPARE: DCMI stop failed, snapshot software guard remains active.");
        }
        else
        {
            Camera_CLI_WriteText(huart, "SNAPSHOT PREPARE: ");
            Camera_CLI_WriteLine(
                huart,
                Camera_SnapshotControl_ErrorToString(result));
        }

        Camera_CLI_PrintSnapshotStatus(huart);
        return CAMERA_CLI_OK;
    }

    if (Camera_CLI_TokenEquals(arg, arg_len, "RESTORE") != 0U)
    {
        result = Camera_SnapshotControl_RequestRestore();

        if (result == CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED)
        {
            Camera_CLI_WriteLine(
                huart,
                "SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.");
        }
        else
        {
            Camera_CLI_WriteText(huart, "SNAPSHOT RESTORE: ");
            Camera_CLI_WriteLine(
                huart,
                Camera_SnapshotControl_ErrorToString(result));
        }

        Camera_CLI_PrintSnapshotStatus(huart);
        return CAMERA_CLI_OK;
    }

    Camera_CLI_WriteLine(huart, "ERR bad SNAPSHOT arg");
    return CAMERA_CLI_ERROR_BAD_ARG;
}

// 处理IWDG故障路径测试命令；仅设置RAM标志，不主动复位
static CameraCliStatus_t Camera_CLI_HandleIwdgTest(UART_HandleTypeDef *huart,
                                                   const char *arg,
                                                   uint32_t arg_len)
{
    if (Camera_CLI_TokenEquals(arg, arg_len, "CAMERA_TIMEOUT") == 0U)
    {
        Camera_CLI_WriteLine(huart, "ERR bad IWDGTEST arg");
        return CAMERA_CLI_ERROR_BAD_ARG;
    }

    Camera_RTOS_EnableIwdgCameraTimeoutTest();
    Camera_CLI_WriteLine(
        huart,
        "IWDG test: CAMERA_TIMEOUT enabled, wait for hardware reset.");
    return CAMERA_CLI_OK;
}

CameraCliStatus_t Camera_CLI_HandleLine(UART_HandleTypeDef *huart, // UART句柄，用于执行命令时向PC输出结果
                                       const char *line)          // 已经由上层文本行解析器整理好的完整命令字符串
{
    const char *trimmed; // 指向去除左侧空格后的命令起始位置
    uint32_t len;        // 保存去除右侧空白后的整行有效长度
    uint32_t cmd_len = 0U; // 保存第一个命令单词的长度，例如 "PROC" 长度为4
    const char *arg;       // 指向命令参数部分，例如 "PROC GRAY" 中的 "GRAY"
    uint32_t arg_len;      // 参数去除首尾空白后的有效长度

    if ((huart == NULL) || (line == NULL)) // 检查UART句柄和命令字符串是否有效
    {
        return CAMERA_CLI_ERROR_NULL; // 任一参数为空时不执行命令，返回空指针错误
    }

    trimmed = Camera_CLI_TrimLeft(line);         // 跳过命令左侧可能存在的空格和制表符
    len = Camera_CLI_TrimmedLength(trimmed);     // 计算去掉右侧空格后的有效长度

    if (len == 0U) // 如果去除首尾空白后没有任何字符，说明这是空行
    {
        return CAMERA_CLI_OK; // 空行直接忽略，不输出错误，也不增加有效命令统计
    }

    Camera_RTOS_RecordCliCommand(); // 记录一次有效的非空文本命令，用于STATUS中的cli_command_count

    while ((cmd_len < len) &&                                   // 确保没有超过整行有效长度
           (Camera_CLI_IsSpace(trimmed[cmd_len]) == 0U))         // 当前字符不是空格或制表符时，仍属于命令名称
    {
        ++cmd_len; // 继续向后寻找命令名称结束位置
    }

    arg = Camera_CLI_TrimLeft(&trimmed[cmd_len]); // 从命令名称结束位置开始，跳过命令和参数之间的空格
    arg_len = Camera_CLI_TrimmedLength(arg);      // 计算参数有效长度，并忽略参数右侧空格

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "HELP") != 0U) // 检查第一个单词是否为HELP
    {
        Camera_CLI_PrintHelp(huart); // 输出全部支持的CLI命令
        return CAMERA_CLI_OK;        // 返回处理成功
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "STATUS") != 0U) // 检查第一个单词是否为STATUS
    {
        Camera_CLI_PrintStatus(huart); // 输出图像配置、RTOS、健康状态和UART DMA统计
        return CAMERA_CLI_OK;          // 返回处理成功
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "PROC") != 0U) // 检查第一个单词是否为PROC
    {
        return Camera_CLI_HandleProc(huart, // 将UART句柄传给PROC子处理函数
                                     arg,   // 传递参数起始地址
                                     arg_len); // 传递参数有效长度，并直接返回子函数执行结果
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "THR") != 0U) // 检查第一个单词是否为THR
    {
        return Camera_CLI_HandleThreshold(huart, // 将阈值查询或设置交给专用子函数
                                          arg,   // 传递THR后的参数
                                          arg_len); // 传递参数长度，并直接返回处理结果
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "SD") != 0U)
    {
        return Camera_CLI_HandleSd(huart, arg, arg_len);
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "SNAPSHOT") != 0U)
    {
        return Camera_CLI_HandleSnapshot(huart, arg, arg_len);
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "IWDGTEST") != 0U)
    {
        return Camera_CLI_HandleIwdgTest(huart, arg, arg_len);
    }

    if (Camera_CLI_TokenEquals(trimmed, cmd_len, "RESET") != 0U) // 检查第一个单词是否为RESET
    {
        Camera_CLI_ResetDefault();              // 将处理模式恢复为BYPASS，将二值化阈值恢复为128
        Camera_CLI_WriteLine(huart, "OK reset"); // 向PC确认配置已经恢复默认值
        return CAMERA_CLI_OK;                   // 返回处理成功
    }

    Camera_RTOS_RecordCliUnknown();                // 当前命令未匹配任何已知命令，增加未知命令统计
    Camera_CLI_WriteLine(huart, "ERR unknown command"); // 输出未知命令错误提示
    return CAMERA_CLI_ERROR_UNKNOWN_CMD;           // 返回未知命令错误状态
}
