# 全工程代码审查与注释整理报告

## 1. 审查范围

本轮在分支 `feature/sd-fatfs-minimal` 上完成 Stage 15A 审查。审查的是开始修改前的 121 个范围文件：

| 范围 | 文件数 | 说明 |
|---|---:|---|
| `Core/**` | 22 | CubeMX 文件只审查并修改 `USER CODE` 区域 |
| `BSPDrivers/**` | 45 | 23 个头文件、22 个源文件 |
| `tools/*.py` | 21 | 不把 `__pycache__/*.pyc` 计入源码文件 |
| `tests/*.c` | 3 | 协议、CRC32、UART dispatcher 测试 |
| 根目录自研文件及 `CMakeLists.txt` | 30 | `CMakeLists.txt` 和修改前已有的 29 个 Markdown 文件 |
| **合计** | **121** | 新增的本报告不反向计入审查基线 |

同时只读检查了下列第三方调用关系，没有修改第三方实现：

- `Drivers/CMSIS/**`
- `Drivers/STM32F4xx_HAL_Driver/**`
- `Middlewares/Third_Party/FreeRTOS/**`
- `Middlewares/Third_Party/FatFs/**`
- `startup_stm32f429xx.s`

开始本轮工作前，工作区已经存在 `STAGE11_SD_SNAPSHOT_PLAN.md` 的修改，以及 4 个未跟踪项目文档；这些用户改动均原样保留，不计入本轮修改统计。

## 2. 发现的问题

| ID | 文件 | 类型 | 严重程度 | 问题 | 是否修改 |
|----|------|------|----------|------|----------|
| A-01 | `BSPDrivers/Src/bsp_PCF8574.c` | STATE | A | I2C 写失败前已经更新影子字节，后续位写可能基于未实际写入硬件的状态 | 是 |
| A-02 | `BSPDrivers/Src/camera_dcmi_dma.c` | STATE | A | 快照超时调用 Stop 后没有清除 active 标志，后续帧中断可能被误判为旧快照完成 | 是 |
| A-03 | `BSPDrivers/Src/OV5640.c` | BOUNDARY | A | 输出尺寸允许 0，图像窗口的 16 位终点计算可能下溢或回绕 | 是 |
| A-04 | `BSPDrivers/Src/OV5640.c` | NULL_CHECK | A | 时序回读接口允许空 `tag` 进入 `%s` 日志路径 | 是 |
| A-05 | `BSPDrivers/Src/camera_sd_storage.c` | BOUNDARY | A | FatFs 磁盘读写只检查指针和 count，未验证 `sector + count` 是否超过卡逻辑块数 | 是 |
| A-06 | `Core/Src/gpio.c` | BOUNDARY | A | LED 枚举非法时会落入 LED1 分支并意外操作硬件 | 是 |
| A-07 | `BSPDrivers/Src/bsp_log.c` | NULL_CHECK | A | `log_printf(NULL)` 会把空格式串传给 `vsnprintf` | 是 |
| A-08 | `tools/pc_dump_rgb565.py` | BOUNDARY | A | 损坏帧头可声明异常尺寸和 payload 长度，在 CRC 校验前触发异常长度读取 | 是 |
| B-01 | `Core/Src/main.c` | OTHER | B | 6 个旧图像处理配置宏在本翻译单元及全工程均无引用，容易误导后续学习 | 是，低风险 |
| B-02 | `bsp_log.c`、`camera_cli.c`、`camera_rtos.c` | ERROR_HANDLING | B | 部分诊断/文本响应的 `HAL_UART_Transmit` 返回值被丢弃 | 否 |
| B-03 | `camera_dcmi_dma.c` | ERROR_HANDLING | B | 已验证的 LCD 直通显示路径中，部分 HAL DCMI/DMA 初始化返回值未向上传递 | 否 |
| B-04 | `delay_tim.c` | ERROR_HANDLING | B | `HAL_TIM_Base_Start` 返回值未向上传递，当前公开初始化接口为 `void` | 否 |
| B-05 | `Core/Src/main.c` | ERROR_HANDLING | B | 部分摄像头/扩展 IO 初始化失败仅记录日志并继续启动 | 否 |
| B-06 | `bsp_log.c`、`camera_cli.c`、`camera_pc_dump.c`、`camera_rtos.c` | TIMEOUT | B | UART 阻塞发送存在 `HAL_MAX_DELAY`，属于无有限 timeout 等待 | 否 |
| C-01 | `Core/Src/*.c`、`camera_rtos.c` | LOOP | C | task 主循环、调度器返回后的循环和 fault Hook 停机循环表面上是无限循环 | 否；任务循环有阻塞/延时，fault 循环为故意停机 |
| C-02 | `camera_sd_storage.c` | LOOP | C | 查找 `IMG0001.BMP` 至 `IMG9999.BMP` 需要有限顺序扫描 | 否；必须保持现有命名规则 |
| C-03 | `camera_cli.c`、`camera_pc_dump.c` | DUPLICATE | C | 存在少量文本裁剪和 UART 文本输出相似代码 | 否；语义和调用上下文不同 |
| C-04 | `tools/*.py` | DUPLICATE | C | 多个历史硬件诊断脚本包含相似串口与 magic 查找逻辑 | 否；各脚本保留独立实验流程和输出格式 |
| C-05 | `CMakeLists.txt` | DUPLICATE | C | BSP 源文件同时显式列出并通过 `GLOB_RECURSE` 收集，存在重复维护点 | 否；当前构建稳定，不为整洁性调整构建模型 |
| C-06 | `camera_rtos.c`、`camera_sd_storage.c` | OTHER | C | 个别函数较长，但包含任务上下文或严格硬件 cleanup 顺序 | 否；禁止在本轮拆分状态机 |
| C-07 | `delay_tim.c`、`bsp_softiic.c` | LOOP | C | 微秒延时和软件 SCCB 使用短时 polling | 否；这是确定长度的硬件时序，不是 task 忙循环 |
| C-08 | `camera_pc_dump.h`、`lcd_mcu.c` | OTHER | C | 保留了历史停用接口说明或已注释诊断实现 | 否；避免触碰历史验证和受保护驱动路径 |
| C-09 | `BSPDrivers/Inc/OV5640cfg.h` | OTHER | C | 编译器对三张旧寄存器表报告 `-Wmissing-braces` | 否；初始化值有效，批量加花括号会制造高风险大 diff |
| D-01 | `BSPDrivers/Inc/*.h`、`Core/Inc/gpio.h` | COMMENT | 仅注释 | public function、enum、struct、重要宏的中文 Doxygen 不完整 | 是 |
| D-02 | `BSPDrivers/Src/*.c`、`Core/Src` USER CODE、`tests/*.c` | COMMENT | 仅注释 | 部分 public/static 函数和关键“为什么”说明不完整 | 是 |
| D-03 | `tools/*.py` | COMMENT | 仅注释 | 部分模块、函数/方法及复杂协议同步逻辑缺少中文说明 | 是 |

汇总：A 级 8 项，B 级 6 项，C 级 9 项；实际修复 9 项（8 个 A 级问题和 1 个极低风险 B 级死配置问题）。未发现未检查的 FatFs `FRESULT`、不必要的 task 忙循环、动态内存、栈上大图像数组或 57600 字节 BMP 全帧缓冲。

## 3. 实际修改

| ID | 修改前问题 | 修改方式 | 为什么安全 | 是否改变行为 |
|---|---|---|---|---|
| A-01 | PCF8574 发送前更新 shadow | 仅在 HAL I2C 写成功后提交 shadow；位写先使用局部候选值 | 正常成功路径写入值和顺序不变 | 仅修正 I2C 失败后的恢复状态 |
| A-02 | DCMI Stop 遗留 snapshot active | Stop 在关闭采集/DMA 后清除 active | 正常快照回调本来已经清零；只影响超时/失败路径 | 仅修正异常路径 |
| A-03 | 0 尺寸及窗口终点回绕 | 首次 SCCB 访问前拒绝 0 尺寸；用 32 位算术验证终点不超过 `0xFFFF` | 所有现有合法分辨率和寄存器写入顺序不变 | 仅拒绝非法参数 |
| A-04 | 空 tag 进入格式化日志 | 空指针直接返回既有失败码 | 现有调用均传固定字符串 | 仅拒绝非法参数 |
| A-05 | SD 扇区范围未检查 | 用 `sector >= LogBlockNbr` 和减法形式检查 count，避免加法溢出 | 有效 FatFs 请求仍进入完全相同的 polling 读写链路 | 仅拒绝越界请求 |
| A-06 | 非法 LED 值操作 LED1 | LED1 分支改为显式枚举判断 | LED0/LED1 的有效电平操作不变 | 非法值改为安全无操作 |
| A-07 | 空日志格式串 | `log_printf` 增加 `fmt == NULL` 保护 | 所有有效日志格式化流程不变 | 仅拒绝非法参数 |
| A-08 | PC 工具信任损坏帧头长度 | 在读取 payload 前固定校验 version、RGB565、160x120 和 38400 字节 | 正式 OV56RGB5 响应字段完全符合这些固定值 | 损坏响应提前报错，正式协议不变 |
| B-01 | 6 个无引用旧宏 | 经全工程引用审计后删除 | 宏没有读取点，不参与任何条件编译或运行逻辑 | 不改变行为 |

注释整理还完成了以下关键说明：

- front/back 双缓冲用于隔离 DCMI 写入与处理/UART 读取。
- DUMP 与 SD SNAPSHOT 共用 `Camera_RTOS_PrepareRgb565Frame()`，保证走同一采集、处理和提交路径。
- SD SNAPSHOT 必须先准备并复制 front frame，再暂停摄像头和接管共享引脚，以缩短图像链路停机时间并获得稳定副本。
- 38400 字节 staging buffer 和 480 字节 BMP 行缓冲均位于文件作用域；BMP 逐行转换，避免 57600 字节 BGR888 全帧缓冲和任务栈压力。
- OV5640 0x3018 屏蔽 D2/D3/D4 是为了避免传感器与 SDIO 同时驱动 PC8/PC9/PC11。
- cleanup 必须先关闭文件和卸载 FatFs，再反初始化 SDIO、关闭时钟、恢复 GPIO/DVP/DCMI；前两步仍依赖卡可访问。
- `SD STATUS` 只复制缓存状态，不触发 SDIO、FatFs 或硬件探测。
- Python 在 `open()` 前设置 DTR/RTS=False，是为了避免 CH340 自动下载电路影响 RESET/BOOT0。
- CRC32 覆盖完整 RGB565 payload，用于发现串口丢字节、错位或数据损坏。

## 4. 未修改项

- 没有删除 `camera_rtos.c/h` 的 UART DMA、StreamBuffer、IWDG、Hook fault、DUMP、binary request、frame id、capture completion 或任务同步状态。
- 没有抽取公共 helper。少量重复代码属于不同协议、硬件状态机或实验脚本上下文。
- 没有改写长函数、camera task、UART DMA、StreamBuffer 或 SD cleanup 状态机。
- 保留了带阻塞/延时的 RTOS 主循环、带 1 ms 延时和 1000 ms timeout 的 SD CardState polling、有限文件名扫描，以及微秒级硬件 polling。
- 保留 UART `HAL_MAX_DELAY` 和部分旧 HAL 返回值处理方式；改变它们需要新增错误传播/协议中止策略，超出“行为不变”范围。
- 所有 FatFs `f_stat`、`f_mount`、`f_open`、`f_write`、`f_close` 返回值及实际写入字节数已经检查，因此没有为形式统一改写现有 cleanup 顺序。
- 保留 `OV5640cfg.h` 旧寄存器表的扁平初始化形式和寄存器值，没有为消除 `-Wmissing-braces` 警告批量改表。
- 保留 CMake 当前显式源文件列表加 glob 的构建方式，没有为去重改变已验证构建模型。

## 5. 注释完成情况

| 自研模块 | 状态 | 说明 |
|---|---|---|
| `camera_rtos` | DONE | 仅补 Doxygen/源文件接口说明；状态和同步逻辑未改 |
| `camera_frame_buffer` | DONE | 说明双缓冲隔离目的和提交约束 |
| `camera_cli` | DONE | 说明 STATUS/SD STATUS 的缓存读取边界，HELP 集合不变 |
| `camera_pc_dump` | DONE | 既有详细协议说明保留，CRC32 和 front frame 关系已明确 |
| `image_request_protocol` | DONE | 二进制请求状态机、timeout 和错误尾部说明完整 |
| `camera_uart_dispatcher` | DONE | 文本/二进制分流和错误尾部隔离原因已说明 |
| `protocol_crc32` | DONE | public API Doxygen 和协议用途完整 |
| `camera_sd_storage` | DONE | staging、BMP 行转换、takeover、cleanup 和只读状态说明完整 |
| `camera_fatfs_diskio` | DONE | FatFs 与 HAL SD polling 适配边界完整 |
| `OV5640` | DONE | public API、返回值和关键寄存器行为完整 |
| `bsp_sccb` | DONE | public API 及软件 SCCB 约束完整 |
| `tools` | DONE | 21/21 模块及全部函数/方法/类有中文 docstring |
| `Core` 自研 USER CODE | DONE | 只在 USER CODE 区域补注释 |
| `tests` | DONE | 3 个测试模块及每个函数均有中文说明 |

## 6. 关键行为确认

| 检查项 | 结果 |
|---|---|
| DUMP 协议 | 未改变 |
| OV56RGB5 帧头、payload、CRC32 算法 | 未改变 |
| binary image request 协议和 dispatcher 状态机 | 未改变 |
| SD SNAPSHOT 外部流程、BMP24 和 IMGxxxx.BMP 规则 | 未改变 |
| SD SNAPSHOT cleanup 顺序 | 未改变 |
| `SD STATUS` | 仍为纯缓存只读 |
| RTOS task 数量、优先级、stack | 未改变 |
| UART DMA / StreamBuffer | 仅补注释，逻辑未改变 |
| OV5640 0x3018 DVP mask 方案 | 未改变 |
| SDIO 模式 | 仍为 1-bit polling，未启用 DMA/IRQ |
| HELP 命令集合 | 仍为 HELP、STATUS、PROC、THR、RESET、DUMP、SD STATUS、SD SNAPSHOT |
| 第三方目录和 startup | `NO_DIFF` |

## 7. 静态检查与后续验证

- `git diff --check`：通过，退出码 0；只有 Git 的 LF→CRLF 工作区提示，没有空白错误。
- `cmake --build build/Debug`：通过，成功生成 `ISP_OV5640.elf`。
- 构建警告：仅见旧 `OV5640cfg.h` 寄存器表的 `-Wmissing-braces`，本轮未改寄存器表结构和值。
- Python：21/21 文件 AST、中文 docstring、重复 docstring 和临时目录 `py_compile` 检查通过。
- RAM：157024 B / 192 KB，79.87%；CCMRAM：0 B / 64 KB。
- FLASH：92256 B / 1 MB，8.80%。
- 本轮按要求没有执行硬件测试，也没有执行 `git commit`。
- 仍需人工回归 DUMP、binary image request、STATUS、SD STATUS、SD SNAPSHOT、BMP 文件可读性、连续多次快照及故障 cleanup 后摄像头恢复。
