#include "camera_cli.h"              // CLI 状态码、运行配置和公开命令接口

#include "camera_frame_buffer.h"     // STATUS 输出固定图像尺寸
#include "camera_rtos.h"             // 读取缓存的任务、Heap、IWDG 和故障统计
#include "camera_sd_storage.h"       // SD STATUS 缓存读取和 SD SNAPSHOT 执行入口
#include "camera_snapshot_control.h" // 初始化 SD takeover 软件保护状态
#include "uart_rx_dma.h"             // 读取 UART DMA 错误、溢出和恢复统计

#include <stddef.h>                   // 提供 NULL 空指针常量

//============================================================================
// @file    camera_cli.c
// @brief   摄像头文本命令解析、运行配置和状态输出模块
//
// camera_pc_dump 在 CommTask 中把 UART 文本累积成完整行。HandleLine() 只识别命令、
// 校验参数并提交 CameraCommand_t，ControlTask 随后从 CommandQueue 取出并调用本模块的
// ExecuteCommand()。PROC/THR/RESET 仍只更新运行配置，下一次公共帧准备才读取这些值。
//
// STATUS 只读取 RTOS/UART 缓存诊断视图；SD STATUS 只复制 SD 状态缓存，不 mount、
// 不初始化 SDIO、不 takeover，也不修改 OV5640 0x3018。只有出队执行 SD SNAPSHOT
// 时才会在 ControlTask 上下文同步完成存储和恢复流程。
// HELP 的八条命令是稳定对外接口，本模块不得擅自增删或改变顺序。
//============================================================================

// 128 是 8 位灰度中点，CLI 初始化和 RESET 都恢复为该默认二值化阈值。
#define CAMERA_CLI_DEFAULT_THRESHOLD 128U

// 运行期配置只由 ControlTask 修改，并由同一任务的下一次帧处理读取。
static CameraCliRuntimeConfig_t s_camera_cli_config;

// 阻塞输出一段 NUL 结尾文本。扫描循环在 '\0' 处退出，不是硬件等待；
// HAL_MAX_DELAY 没有模块级软件 timeout，输出失败不会改变命令业务返回结果。
static void Camera_CLI_WriteText(UART_HandleTypeDef *huart, const char *text)
{
    const char *cursor = text;  // 从字符串首地址向 NUL 终止符移动
    uint16_t length = 0U;       // 传给 HAL 的实际文本字节数，不包含 NUL

    if ((huart == NULL) || (text == NULL))
    {
        return;
    }

    while (*cursor != '\0')
    {
        ++cursor;
        ++length;
    }

    if (length != 0U)
    {
        (void)HAL_UART_Transmit(
            huart,
            (uint8_t *)text,
            length,
            HAL_MAX_DELAY);
    }
}

// 通过 UART 输出文本并追加 CRLF
static void Camera_CLI_WriteLine(UART_HandleTypeDef *huart, const char *text)
{
    Camera_CLI_WriteText(huart, text);
    Camera_CLI_WriteText(huart, "\r\n");
}

// 将 uint32_t 逆序写入 11 字节缓冲：最多 10 位十进制数字加 NUL。
// do-while 保证数值 0 也输出一位；value 归零或到达缓冲起点时退出，循环有界。
static void Camera_CLI_WriteU32(UART_HandleTypeDef *huart, uint32_t value)
{
    char buffer[11];
    uint32_t position = sizeof(buffer);

    buffer[--position] = '\0';
    do
    {
        buffer[--position] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (position > 0U));

    Camera_CLI_WriteText(huart, &buffer[position]);
}

// 输出名称为文本、值为无符号整数的状态字段
static void Camera_CLI_WriteFieldU32(
    UART_HandleTypeDef *huart,
    const char *name,
    uint32_t value)
{
    Camera_CLI_WriteText(huart, "  ");
    Camera_CLI_WriteText(huart, name);
    Camera_CLI_WriteText(huart, "=");
    Camera_CLI_WriteU32(huart, value);
    Camera_CLI_WriteText(huart, "\r\n");
}

// 输出名称和值均为文本的状态字段
static void Camera_CLI_WriteFieldText(
    UART_HandleTypeDef *huart,
    const char *name,
    const char *value)
{
    Camera_CLI_WriteText(huart, "  ");
    Camera_CLI_WriteText(huart, name);
    Camera_CLI_WriteText(huart, "=");
    Camera_CLI_WriteLine(huart, value);
}

// 将 ASCII 小写字母转换为大写，支持命令不区分大小写
static char Camera_CLI_ToUpper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char)(ch - ('a' - 'A'));
    }

    return ch;
}

// 判断字符是否为 CLI 接受的空白字符
static uint8_t Camera_CLI_IsSpace(char ch)
{
    return ((ch == ' ') || (ch == '\t')) ? 1U : 0U;
}

// 跳过 CLI 接受的空格/TAB；遇到非空白或 NUL 即退出，不涉及硬件 timeout。
static const char *Camera_CLI_TrimLeft(const char *text)
{
    while ((text != NULL) && (Camera_CLI_IsSpace(*text) != 0U))
    {
        ++text;
    }

    return text;
}

// 先扫描到 NUL，再在已知长度范围内向前剔除空格/TAB；两个循环都受字符串边界约束。
static uint32_t Camera_CLI_TrimmedLength(const char *text)
{
    uint32_t length = 0U;

    if (text == NULL)
    {
        return 0U;
    }

    while (text[length] != '\0')
    {
        ++length;
    }

    while ((length > 0U) &&
           (Camera_CLI_IsSpace(text[length - 1U]) != 0U))
    {
        --length;
    }

    return length;
}

// 在固定命令 token 范围内做 ASCII 大小写无关比较，长度不足或首个不匹配立即失败。
static uint8_t Camera_CLI_TokenEquals(
    const char *text,
    uint32_t length,
    const char *token)
{
    uint32_t index = 0U;

    while (token[index] != '\0')
    {
        if ((index >= length) ||
            (Camera_CLI_ToUpper(text[index]) != token[index]))
        {
            return 0U;
        }

        ++index;
    }

    return (index == length) ? 1U : 0U;
}

// 在 argument_length 固定上界内解析十进制 0～255；非数字或累计值超 255 立即失败。
// 只有整段全部合法才写出 value，因此错误参数不会修改现有 CLI 配置。
static uint8_t Camera_CLI_ParseU8(
    const char *text,
    uint32_t length,
    uint8_t *value)
{
    uint32_t parsed = 0U;
    uint32_t index;

    if ((text == NULL) || (value == NULL) || (length == 0U))
    {
        return 0U;
    }

    for (index = 0U; index < length; ++index)
    {
        if ((text[index] < '0') || (text[index] > '9'))
        {
            return 0U;
        }

        parsed = (parsed * 10U) + (uint32_t)(text[index] - '0');
        if (parsed > 255U)
        {
            return 0U;
        }
    }

    *value = (uint8_t)parsed;
    return 1U;
}

// 返回图像处理模式对应的状态文本
static const char *Camera_CLI_ModeName(CameraProcessMode_t mode)
{
    if (mode == CAMERA_PROCESS_MODE_BYPASS)
    {
        return "BYPASS";
    }
    if (mode == CAMERA_PROCESS_MODE_GRAYSCALE)
    {
        return "GRAYSCALE";
    }
    if (mode == CAMERA_PROCESS_MODE_BINARY)
    {
        return "BINARY";
    }

    return "UNKNOWN";
}

// 输出对外固定的八条 HELP 命令，顺序和文本都是现有串口接口的一部分。
// DUMP 虽列在 HELP 中，但由上游 camera_pc_dump 识别并转成事件，不在 HandleLine 执行。
static void Camera_CLI_PrintHelp(UART_HandleTypeDef *huart)
{
    Camera_CLI_WriteLine(huart, "HELP");
    Camera_CLI_WriteLine(huart, "STATUS");
    Camera_CLI_WriteLine(huart, "PROC [BYPASS|GRAY|BINARY]");
    Camera_CLI_WriteLine(huart, "THR [0..255]");
    Camera_CLI_WriteLine(huart, "RESET");
    Camera_CLI_WriteLine(huart, "DUMP");
    Camera_CLI_WriteLine(huart, "SD STATUS");
    Camera_CLI_WriteLine(huart, "SD SNAPSHOT");
}

// 输出缓存的 RTOS、内存、故障和看门狗健康状态。
// 两个 stats 指针都是实时诊断视图而非原子快照；NULL 时用 0 降级输出，保证 STATUS
// 自身不会因统计不可用而失败。本函数不启动 DCMI、SDIO 或 FatFs 操作。
static void Camera_CLI_PrintStatus(UART_HandleTypeDef *huart)
{
    const CameraRtosStats_t *stats = Camera_RTOS_GetStats();
    const UartRxDmaStats_t *uart_stats = UART_RxDma_GetStats();

    Camera_CLI_WriteLine(huart, "STATUS:");
    Camera_CLI_WriteFieldText(
        huart,
        "mode",
        Camera_CLI_ModeName(s_camera_cli_config.process_mode));
    Camera_CLI_WriteFieldU32(
        huart,
        "threshold",
        s_camera_cli_config.binary_threshold);
    Camera_CLI_WriteText(huart, "  frame=");
    Camera_CLI_WriteU32(huart, CAMERA_FB_WIDTH);
    Camera_CLI_WriteText(huart, "x");
    Camera_CLI_WriteU32(huart, CAMERA_FB_HEIGHT);
    Camera_CLI_WriteText(huart, "\r\n");
    Camera_CLI_WriteFieldText(
        huart,
        "tuning",
        "AEC_BASELINE,AWB_AUTO,B+1,C0,S1,SH0");
    Camera_CLI_WriteFieldU32(
        huart,
        "uptime_ms",
        (stats != NULL) ? stats->uptime_ms : 0U);

    Camera_CLI_WriteLine(huart, "HEALTH:");
    Camera_CLI_WriteFieldU32(
        huart,
        "heap_free",
        (stats != NULL) ? stats->free_heap_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "heap_min",
        (stats != NULL) ? stats->min_ever_free_heap_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_comm_min",
        (stats != NULL) ? stats->comm_stack_min_free_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_control_min",
        (stats != NULL) ? stats->control_stack_min_free_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_capture_min",
        (stats != NULL) ? stats->capture_stack_min_free_bytes : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stack_monitor_min",
        (stats != NULL) ? stats->monitor_stack_min_free_bytes : 0U);

    Camera_CLI_WriteLine(huart, "FAULT:");
    Camera_CLI_WriteFieldU32(
        huart,
        "hook_fault",
        (stats != NULL) ? stats->hook_fault_code : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "assert_line",
        (stats != NULL) ? stats->assert_line : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "uart_dma_error",
        (uart_stats != NULL) ? uart_stats->uart_error_count : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "stream_overflow",
        (uart_stats != NULL) ? uart_stats->stream_overflow_bytes : 0U);

    Camera_CLI_WriteLine(huart, "IWDG:");
    Camera_CLI_WriteFieldU32(
        huart,
        "enabled",
        (stats != NULL) ? stats->iwdg_enabled : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "refresh_skip",
        (stats != NULL) ? stats->iwdg_refresh_skip_count : 0U);
    Camera_CLI_WriteFieldU32(
        huart,
        "last_skip_reason",
        (stats != NULL) ? stats->iwdg_last_skip_reason : 0U);
}

// 只把 SD 模块缓存复制到局部 status 后格式化输出。
// 该查询不 mount、不 HAL_SD_Init、不 takeover、不改 0x3018，避免状态查询打断图像链路。
static void Camera_CLI_PrintSdStatus(UART_HandleTypeDef *huart)
{
    CameraSdStorageStatus_t status;

    Camera_SDStorage_GetStatus(&status);
    Camera_CLI_WriteLine(huart, "SD STATUS:");
    Camera_CLI_WriteFieldU32(huart, "supported", status.supported);
    Camera_CLI_WriteFieldU32(huart, "card_ready", status.card_ready);
    Camera_CLI_WriteFieldU32(
        huart,
        "takeover_required",
        status.takeover_required);
    Camera_CLI_WriteFieldU32(huart, "sdio_ready", status.sdio_ready);
    Camera_CLI_WriteFieldU32(huart, "fatfs_ready", status.fatfs_ready);
    Camera_CLI_WriteFieldText(huart, "last_mount", status.last_mount_text);
    Camera_CLI_WriteFieldText(
        huart,
        "last_snapshot",
        status.last_snapshot_text);
    Camera_CLI_WriteFieldText(huart, "last_file", status.last_file_name);
    Camera_CLI_WriteFieldU32(
        huart,
        "last_file_size",
        status.last_file_size);
    Camera_CLI_WriteFieldU32(huart, "save_count", status.save_count);
    Camera_CLI_WriteFieldText(huart, "save_error", status.save_error_text);
    Camera_CLI_WriteFieldText(huart, "last_error", status.last_error_text);
    Camera_CLI_WriteFieldU32(huart, "last_total_ms", status.last_total_ms);
    Camera_CLI_WriteFieldU32(huart, "last_write_ms", status.last_write_ms);
    Camera_CLI_WriteFieldText(
        huart,
        "dvp_mask_solution",
        "OV5640_3018_6_4");
}

// 在 ControlTask 中同步执行一次完整 SD SNAPSHOT，并输出各阶段诊断字段。
// 存储模块负责 prepare/takeover/FatFs/cleanup；CLI 只展示统一结果，不复制状态机逻辑。
static CameraCliStatus_t Camera_CLI_RunSdSnapshot(
    UART_HandleTypeDef *huart)
{
    CameraSdSnapshotResult_t snapshot_result;  // 保存文件、耗时及各阶段结果的输出结构
    uint32_t result;  // 整个保存流程的最终错误码

    result = Camera_SDStorage_SaveSnapshotFrame(&snapshot_result);
    Camera_CLI_WriteLine(huart, "SD SNAPSHOT:");
    Camera_CLI_WriteFieldText(
        huart,
        "result",
        (result == CAMERA_SD_OK) ? "PASS" : "FAIL");
    Camera_CLI_WriteFieldText(huart, "file", snapshot_result.file_name);
    Camera_CLI_WriteFieldU32(
        huart,
        "bytes",
        snapshot_result.bytes_written);
    Camera_CLI_WriteFieldText(huart, "source", snapshot_result.source_text);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_bytes",
        snapshot_result.source_bytes);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_nonzero",
        snapshot_result.source_nonzero);
    Camera_CLI_WriteFieldU32(
        huart,
        "source_sum32",
        snapshot_result.source_sum32);
    Camera_CLI_WriteFieldText(
        huart,
        "prepare",
        snapshot_result.prepare_text);
    Camera_CLI_WriteFieldU32(
        huart,
        "prepare_retry",
        snapshot_result.prepare_retry);
    Camera_CLI_WriteFieldText(huart, "format", snapshot_result.format_text);
    Camera_CLI_WriteFieldU32(huart, "width", snapshot_result.width);
    Camera_CLI_WriteFieldU32(huart, "height", snapshot_result.height);
    Camera_CLI_WriteFieldText(huart, "mount", snapshot_result.mount_text);
    Camera_CLI_WriteFieldText(huart, "write", snapshot_result.write_text);
    Camera_CLI_WriteFieldText(
        huart,
        "cleanup",
        snapshot_result.cleanup_text);
    Camera_CLI_WriteFieldText(
        huart,
        "restore",
        snapshot_result.restore_text);
    Camera_CLI_WriteFieldU32(huart, "total_ms", snapshot_result.total_ms);
    Camera_CLI_WriteFieldU32(huart, "prepare_ms", snapshot_result.prepare_ms);
    Camera_CLI_WriteFieldU32(huart, "write_ms", snapshot_result.write_ms);
    Camera_CLI_WriteFieldU32(huart, "cleanup_ms", snapshot_result.cleanup_ms);
    if (result != CAMERA_SD_OK)
    {
        Camera_CLI_WriteFieldText(
            huart,
            "error",
            snapshot_result.error_text);
        return CAMERA_CLI_ERROR;
    }

    return CAMERA_CLI_OK;
}

// 统一提交 parser 已完整构造的值对象；CommTask 不在这里进行 UART TX。
static CameraCliStatus_t Camera_CLI_SubmitCommand(
    const CameraCommand_t *command)
{
    if (Camera_CommandSubmit(command) == false)
    {
        return CAMERA_CLI_ERROR_QUEUE_FULL;
    }

    return CAMERA_CLI_OK;
}

// 将既有 parser 错误也作为值对象提交，使所有 UART 文本只由 ControlTask 输出。
static CameraCliStatus_t Camera_CLI_SubmitError(
    CameraCommandCliError_t error,
    CameraCliStatus_t parser_status)
{
    CameraCommand_t command = {0};

    command.type = CAMERA_CMD_CLI_ERROR;
    command.args.cli_error.error = error;
    return (Camera_CLI_SubmitCommand(&command) == CAMERA_CLI_OK) ?
        parser_status : CAMERA_CLI_ERROR_QUEUE_FULL;
}

// 恢复 BYPASS/128 默认配置；只改缓存，不立即处理图像或启动 DCMI。
void Camera_CLI_ResetDefault(void)
{
    s_camera_cli_config.process_mode = CAMERA_PROCESS_MODE_BYPASS;
    s_camera_cli_config.binary_threshold = CAMERA_CLI_DEFAULT_THRESHOLD;
}

// 启动阶段初始化 CLI、SD 状态缓存和 takeover guard；不会初始化 SDIO 或挂载 FatFs。
void Camera_CLI_Init(void)
{
    Camera_CLI_ResetDefault();
    Camera_SDStorage_InitState();
    Camera_SnapshotControl_InitState();
}

// 返回下一次 Camera_RTOS_PrepareRgb565Frame 将读取的图像处理模式。
CameraProcessMode_t Camera_CLI_GetProcessMode(void)
{
    return s_camera_cli_config.process_mode;
}

// 返回下一次 BINARY 帧处理将读取的 8 位灰度阈值。
uint8_t Camera_CLI_GetBinaryThreshold(void)
{
    return s_camera_cli_config.binary_threshold;
}

// 返回模块静态配置的只读视图；指针长期有效，但后续 CLI 命令会更新其内容。
const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void)
{
    return &s_camera_cli_config;
}

// 在 ControlTask 中执行已经出队的 CLI 命令；原有输出和业务 helper 保持复用。
CameraCliStatus_t Camera_CLI_ExecuteCommand(
    UART_HandleTypeDef *huart,
    const CameraCommand_t *command)
{
    if ((huart == NULL) || (command == NULL))
    {
        return CAMERA_CLI_ERROR_NULL;
    }

    switch (command->type)
    {
        case CAMERA_CMD_HELP:
            Camera_CLI_PrintHelp(huart);
            return CAMERA_CLI_OK;

        case CAMERA_CMD_STATUS:
            Camera_CLI_PrintStatus(huart);
            return CAMERA_CLI_OK;

        case CAMERA_CMD_CLI_ERROR:
            if (command->args.cli_error.error ==
                CAMERA_COMMAND_CLI_ERROR_BAD_PROC_ARG)
            {
                Camera_CLI_WriteLine(huart, "ERR bad PROC arg");
                return CAMERA_CLI_ERROR_BAD_ARG;
            }
            if (command->args.cli_error.error ==
                CAMERA_COMMAND_CLI_ERROR_BAD_THRESHOLD_ARG)
            {
                Camera_CLI_WriteLine(huart, "ERR bad THR arg");
                return CAMERA_CLI_ERROR_BAD_ARG;
            }
            Camera_CLI_WriteLine(huart, "ERR unknown command");
            return CAMERA_CLI_ERROR_UNKNOWN_CMD;

        case CAMERA_CMD_PROC_GET:
            Camera_CLI_WriteText(huart, "process mode: ");
            Camera_CLI_WriteLine(
                huart,
                Camera_CLI_ModeName(s_camera_cli_config.process_mode));
            return CAMERA_CLI_OK;

        case CAMERA_CMD_PROC_SET:
            if ((command->args.proc.mode != CAMERA_PROCESS_MODE_BYPASS) &&
                (command->args.proc.mode != CAMERA_PROCESS_MODE_GRAYSCALE) &&
                (command->args.proc.mode != CAMERA_PROCESS_MODE_BINARY))
            {
                Camera_CLI_WriteLine(huart, "ERR bad PROC arg");
                return CAMERA_CLI_ERROR_BAD_ARG;
            }
            s_camera_cli_config.process_mode = command->args.proc.mode;
            Camera_CLI_WriteText(huart, "OK process mode: ");
            Camera_CLI_WriteLine(
                huart,
                Camera_CLI_ModeName(s_camera_cli_config.process_mode));
            return CAMERA_CLI_OK;

        case CAMERA_CMD_THRESHOLD_GET:
            Camera_CLI_WriteText(huart, "binary threshold: ");
            Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
            Camera_CLI_WriteText(huart, "\r\n");
            return CAMERA_CLI_OK;

        case CAMERA_CMD_THRESHOLD_SET:
            s_camera_cli_config.binary_threshold =
                command->args.threshold.threshold;
            Camera_CLI_WriteText(huart, "OK binary threshold: ");
            Camera_CLI_WriteU32(huart, s_camera_cli_config.binary_threshold);
            Camera_CLI_WriteText(huart, "\r\n");
            return CAMERA_CLI_OK;

        case CAMERA_CMD_RESET:
            Camera_CLI_ResetDefault();
            Camera_CLI_WriteLine(huart, "OK reset");
            return CAMERA_CLI_OK;

        case CAMERA_CMD_SD_STATUS:
            Camera_CLI_PrintSdStatus(huart);
            return CAMERA_CLI_OK;

        case CAMERA_CMD_SD_SNAPSHOT:
            return Camera_CLI_RunSdSnapshot(huart);

        default:
            return CAMERA_CLI_ERROR_UNKNOWN_CMD;
    }
}

// 解析一行完整、NUL 结尾的 CLI 文本并生成不含临时指针的 CameraCommand_t。
// 上游正常行缓冲最多保存 31 个字符；DUMP 已在上游转换为命令事件。
CameraCliStatus_t Camera_CLI_HandleLine(
    UART_HandleTypeDef *huart,
    const char *line)
{
    const char *trimmed;  // 跳过行首空白后的命令起点
    const char *argument; // 第一个空白之后再次跳过空白的参数起点
    uint32_t length;      // 去除首尾空白后剩余文本的有效长度
    uint32_t command_length = 0U;  // 第一个空白前的命令关键字长度
    uint32_t argument_length;      // 去除参数尾部空白后的长度
    CameraCommand_t command = {0}; // 仅保存执行所需枚举和小参数，提交时按值复制

    if ((huart == NULL) || (line == NULL))
    {
        return CAMERA_CLI_ERROR_NULL;
    }

    trimmed = Camera_CLI_TrimLeft(line);
    length = Camera_CLI_TrimmedLength(trimmed);
    if (length == 0U)
    {
        return CAMERA_CLI_OK;
    }

    // 最多扫描 length 个字符寻找第一个空白；命令行有明确缓冲上界，不会无界循环。
    while ((command_length < length) &&
           (Camera_CLI_IsSpace(trimmed[command_length]) == 0U))
    {
        ++command_length;
    }

    argument = Camera_CLI_TrimLeft(&trimmed[command_length]);
    argument_length = Camera_CLI_TrimmedLength(argument);

    if (Camera_CLI_TokenEquals(trimmed, command_length, "HELP") != 0U)
    {
        command.type = CAMERA_CMD_HELP;
        return Camera_CLI_SubmitCommand(&command);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "STATUS") != 0U)
    {
        command.type = CAMERA_CMD_STATUS;
        return Camera_CLI_SubmitCommand(&command);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "PROC") != 0U)
    {
        if (argument_length == 0U)
        {
            command.type = CAMERA_CMD_PROC_GET;
        }
        else if (Camera_CLI_TokenEquals(
                     argument,
                     argument_length,
                     "BYPASS") != 0U)
        {
            command.type = CAMERA_CMD_PROC_SET;
            command.args.proc.mode = CAMERA_PROCESS_MODE_BYPASS;
        }
        else if ((Camera_CLI_TokenEquals(
                      argument,
                      argument_length,
                      "GRAY") != 0U) ||
                 (Camera_CLI_TokenEquals(
                      argument,
                      argument_length,
                      "GRAYSCALE") != 0U))
        {
            command.type = CAMERA_CMD_PROC_SET;
            command.args.proc.mode = CAMERA_PROCESS_MODE_GRAYSCALE;
        }
        else if (Camera_CLI_TokenEquals(
                     argument,
                     argument_length,
                     "BINARY") != 0U)
        {
            command.type = CAMERA_CMD_PROC_SET;
            command.args.proc.mode = CAMERA_PROCESS_MODE_BINARY;
        }
        else
        {
            return Camera_CLI_SubmitError(
                CAMERA_COMMAND_CLI_ERROR_BAD_PROC_ARG,
                CAMERA_CLI_ERROR_BAD_ARG);
        }

        return Camera_CLI_SubmitCommand(&command);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "THR") != 0U)
    {
        if (argument_length == 0U)
        {
            command.type = CAMERA_CMD_THRESHOLD_GET;
        }
        else
        {
            command.type = CAMERA_CMD_THRESHOLD_SET;
            if (Camera_CLI_ParseU8(
                    argument,
                    argument_length,
                    &command.args.threshold.threshold) == 0U)
            {
                return Camera_CLI_SubmitError(
                    CAMERA_COMMAND_CLI_ERROR_BAD_THRESHOLD_ARG,
                    CAMERA_CLI_ERROR_BAD_ARG);
            }
        }

        return Camera_CLI_SubmitCommand(&command);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "RESET") != 0U)
    {
        command.type = CAMERA_CMD_RESET;
        return Camera_CLI_SubmitCommand(&command);
    }
    if (Camera_CLI_TokenEquals(trimmed, command_length, "SD") != 0U)
    {
        if (Camera_CLI_TokenEquals(argument, argument_length, "STATUS") != 0U)
        {
            command.type = CAMERA_CMD_SD_STATUS;
            return Camera_CLI_SubmitCommand(&command);
        }
        if (Camera_CLI_TokenEquals(
                argument,
                argument_length,
                "SNAPSHOT") != 0U)
        {
            command.type = CAMERA_CMD_SD_SNAPSHOT;
            return Camera_CLI_SubmitCommand(&command);
        }

        return Camera_CLI_SubmitError(
            CAMERA_COMMAND_CLI_ERROR_UNKNOWN,
            CAMERA_CLI_ERROR_UNKNOWN_CMD);
    }

    return Camera_CLI_SubmitError(
        CAMERA_COMMAND_CLI_ERROR_UNKNOWN,
        CAMERA_CLI_ERROR_UNKNOWN_CMD);
}
