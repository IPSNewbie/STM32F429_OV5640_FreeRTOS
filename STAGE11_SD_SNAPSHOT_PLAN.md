# Stage 11：SD 卡拍照保存方案预研

## 1. 文档目的与范围

本文档用于规划 STM32F429 + OV5640 项目的 SD 卡拍照保存方案，分析硬件约束、移植参考、实施阶段、工程取舍和主要风险。

Stage 11A 只进行方案设计和风险分析，不修改 STM32 固件、不新增 C/H 源码，也不修改 Python 工具、UART 协议、DCMI、SDIO 或 FATFS 实现。

当前项目已经完成以下能力：

1. STM32F429 + OV5640 图像采集。
2. LCD 显示。
3. PC DUMP 图像导出。
4. 基础图像处理。
5. UART CLI。
6. FreeRTOS 多任务化。
7. UART DMA + StreamBuffer。
8. 二进制图像请求协议。
9. IWDG 看门狗。
10. 500 次连续图像请求稳定性测试。

Stage 11 的目标是在上述稳定版本之上验证 SD 卡拍照保存能力，而不是重构或替换已有图像链路。

## 2. 硬件接口与关键约束

正点原子阿波罗 STM32F429 开发板的 SD 卡接口使用 STM32F4 自带的 SDIO 外设，配套 SD 卡实验文档使用 Micro SD 卡接口。SDIO 支持 1 位和 4 位数据线，阿波罗实验重点采用 SDIO 方式。

STM32F4 的 SDIO 初始化阶段，SDIO_CK 不能超过 400 kHz；只有在卡初始化完成后，才能提高 SDIO 时钟以提升数据传输速度。后续移植必须保留“低速初始化、完成后提速”的时钟切换过程。

### 2.1 DCMI / OV5640 引脚

| 摄像头信号 | MCU 引脚 | DCMI 功能 |
| --- | --- | --- |
| OV_D2 | PC8 | DCMI_D2 |
| OV_D3 | PC9 | DCMI_D3 |
| OV_D4 | PC11 | DCMI_D4 |

### 2.2 SDIO 引脚

| SDIO 信号 | MCU 引脚 |
| --- | --- |
| SDIO_D0 | PC8 |
| SDIO_D1 | PC9 |
| SDIO_D2 | PC10 |
| SDIO_D3 | PC11 |
| SDIO_CK | PC12 |
| SDIO_CMD | PD2 |

### 2.3 引脚冲突结论

DCMI 与 SDIO 在 PC8、PC9、PC11 上存在直接的复用冲突：

- PC8 同时承担 DCMI_D2 和 SDIO_D0。
- PC9 同时承担 DCMI_D3 和 SDIO_D1。
- PC11 同时承担 DCMI_D4 和 SDIO_D3。

同一时刻这些引脚只能配置给一个外设。因此，当前硬件连接下不能简单实现“DCMI 连续采集 + SDIO 同时写卡”，也不能把 SD 卡写入直接加入现有连续采集路径。推荐采用互斥、分时的“拍照式保存”：先完成一帧采集并停止 DCMI/DMA，再切换冲突引脚给 SDIO 完成文件写入；如需继续采集，再恢复引脚和 DCMI。

## 3. 正点原子 SD 卡实验参考流程

正点原子 SD 卡实验中可借鉴的流程和组织方式如下：

1. 先完成系统初始化。
2. 随后初始化 LED、LCD、按键、SDRAM 等用户外设。
3. 用户外设就绪后执行 SD 卡初始化。
4. SD 卡初始化成功后读取并打印 SD 卡信息，便于确认卡类型、容量和初始化状态。
5. 通过按键触发读扇区测试，将初始化验证与数据读写验证分开。
6. SDIO 驱动主要由 `sdio_sdcard.c` 和 `sdio_sdcard.h` 组成，可作为后续独立移植或接口设计参考。
7. SDIO 信号线通过宏定义集中管理，便于移植时核对和调整 GPIO 配置。
8. SDIO 初始化阶段的时钟不能超过 400 kHz。
9. 卡初始化完成后再提高 SDIO 时钟，进入正常数据传输阶段。

借鉴上述实验时，应只提取适合当前 HAL、FreeRTOS 和现有工程结构的初始化顺序及卡访问逻辑，不应直接覆盖当前项目已经验证稳定的系统、GPIO、时钟或中断配置。

## 4. Stage 11 分阶段路线

### Stage 11A：方案预研文档

本阶段只写文档，不修改代码。

目标：

- 分析正点原子 SD 卡实验流程。
- 分析 SDIO 与 DCMI 引脚冲突。
- 确定拍照式保存路线。
- 明确后续代码阶段边界。

### Stage 11B：SD 卡独立初始化验证

目标：

- 暂时不启用 DCMI 采集。
- 移植或参考 SDIO 初始化。
- 验证 SD 卡能否初始化。
- 验证能否读取卡信息。
- 验证基础扇区读写或文件写入。

阶段边界与保护原则：

- 本阶段只验证 SD 卡。
- 不接入摄像头保存。
- 不影响现有 UART DUMP。
- 不破坏 Stage 10 稳定性代码。
- 优先通过独立测试入口验证，避免将尚未稳定的 SDIO 流程插入实时任务。

### Stage 11C：FATFS 文件系统写文件

目标：

- 接入 FATFS。
- 挂载 SD 卡。
- 创建测试文件。
- 写入文本或固定二进制数据。
- 读回并校验写入内容。
- 通过 CLI 查询测试结果。

本阶段仍不接入摄像头帧保存，先独立验证文件系统挂载、创建、写入、同步、关闭和读回流程，并记录明确的错误码。

### Stage 11D：单帧图像保存方案验证

推荐流程：

1. 通过现有摄像头链路采集一帧到 frame buffer。
2. 确认该帧已成为可稳定读取的 front buffer。
3. 停止 DCMI。
4. 停止相关 DMA，并确认传输已经停止。
5. 将冲突引脚 PC8、PC9、PC11 从 DCMI 释放。
6. 将相关引脚重新配置为 SDIO。
7. 初始化或恢复 SDIO，并挂载 FATFS。
8. 将 front buffer 保存为文件。
9. 同步并关闭文件，记录文件名、写入字节数、耗时和错误码。
10. 卸载 FATFS，或在确认安全的前提下保持文件系统逻辑状态。
11. 如需继续采集，停止 SDIO 对冲突引脚的使用。
12. 将 PC8、PC9、PC11 恢复为 DCMI 功能。
13. 恢复相关 DMA 配置并重新启动 DCMI 采集。

引脚切换前后必须有明确的外设停止、状态确认和错误回滚流程，不能只切换 GPIO 复用功能而忽略 DCMI、DMA、SDIO 和 FATFS 的状态。

### 4.1 文件格式建议

第一阶段优先保存 raw RGB565，例如：

```text
snapshot_0001_160x120_rgb565.raw
```

raw 文件不需要编码器和复杂文件头，写入内容可以直接对应 frame buffer，最简单、风险最低，也便于使用现有 PC 工具离线检查。

后续再考虑 BMP，例如：

```text
snapshot_0001_160x120_rgb565.bmp
```

BMP 支持应作为后续优化单独验证，因为需要增加文件头，并正确处理 RGB565 像素格式、字节序、颜色掩码和行对齐，不能与首轮 SD 卡链路验证同时引入。

## 5. 与当前项目结合的工程取舍

1. 当前项目已经有稳定的 PC DUMP 导出链路，仍应作为图像导出、分析和回归验证的基准能力。
2. SD 卡保存不是替代 PC DUMP，而是在无法连接 PC 或需要本地留存时提供扩展功能。
3. SD 卡保存优先实现“拍照式保存”，本阶段及近期阶段不做实时录像。
4. 不追求连续高速写卡，首要目标是单帧保存正确、可恢复、可诊断。
5. 不做 DCMI 和 SDIO 并行；两个外设对冲突引脚的使用必须互斥。
6. 不改变或破坏现有 UART 协议；新增能力应通过兼容的 CLI 扩展入口提供。
7. 不破坏 FreeRTOS/IWDG 稳定性，耗时 SD 卡操作不得阻塞高优先级实时路径。
8. 不破坏已经通过 500 次连续图像请求长测的稳定版本，后续每阶段都应进行相关回归验证。

## 6. 后续 CLI 命令规划

本节只规划接口语义，不在 Stage 11A 实现命令或修改 UART 协议。

| 命令 | 规划含义 |
| --- | --- |
| `SD STATUS` | 查询 SD 卡模块状态，包括 SDIO、卡检测、初始化和 FATFS 挂载状态。 |
| `SD INIT` | 初始化 SDIO 和 SD 卡，并返回卡信息或明确的失败阶段及错误码。 |
| `SD TEST` | 写入测试文件并读回验证，报告文件名、字节数、耗时和校验结果。 |
| `SNAPSHOT SD` | 拍摄当前一帧，并按互斥切换流程将图像保存到 SD 卡。 |
| `SNAPSHOT STATUS` | 查询最近一次保存结果、文件名、写入字节数、耗时和错误码。 |

命令处理应采用异步状态或低优先级工作任务承载耗时操作，避免 CLI 接收路径、高优先级实时任务或关键看门狗路径被长时间阻塞。具体任务模型留到对应代码阶段评审。

## 7. 风险列表与保护原则

### 7.1 主要风险

1. **PC8、PC9、PC11 引脚复用冲突**：DCMI 与 SDIO 不能同时占用这些引脚，切换顺序错误可能导致采集或写卡失败。
2. **DCMI 停止和恢复流程复杂**：需要协调帧边界、DMA 状态、缓冲区所有权、中断和错误恢复；恢复不完整可能导致后续图像错位或停止采集。
3. **SDIO 初始化影响现有 GPIO 配置**：直接套用示例初始化代码可能覆盖 DCMI 或其他已验证配置。
4. **FATFS 增加 RAM 和 Flash 占用**：文件系统对象、扇区缓存、长文件名配置和驱动代码都需要进行资源评估。
5. **写卡耗时不可控**：SD 卡内部擦写和整理会造成延迟抖动，写卡不能放在高优先级实时路径中。
6. **IWDG 覆盖长时间写卡场景**：写文件、同步或卡异常可能长时间阻塞，需要确保看门狗喂狗策略仍由健康任务和有效状态驱动，不能用无条件喂狗掩盖死锁。
7. **SD 卡兼容性差异**：不同品牌、容量、速度等级和卡状态会影响初始化成功率及写入速度，需要准备多卡测试和超时处理。
8. **异常断电导致文件损坏**：未完成同步、目录项更新或文件关闭时掉电，可能损坏当前文件，严重时可能影响文件系统。
9. **BMP 格式转换风险**：后续保存 BMP 时，需要正确处理 RGB565 与 BMP 格式、像素字节序、颜色掩码和每行对齐。

### 7.2 保护原则

- 每次切换外设前确认当前外设已经停止，并定义失败后的可恢复状态。
- 保持 frame buffer 所有权明确，写卡期间禁止采集链路覆盖正在保存的 front buffer。
- SDIO/GPIO 配置采用封装接口，禁止在不同模块散落直接寄存器修改。
- 耗时写卡放在适当优先级的任务中，并为初始化、读写、同步和关闭设置超时及错误码。
- 评估 FATFS 的静态和动态资源占用，避免栈、堆或缓冲区不足。
- 保存时采用“写入—同步—关闭—报告结果”的完整流程；异常退出时尽量关闭文件并恢复外设状态。
- Stage 11B、11C、11D 每阶段独立验证，问题未收敛前不进入下一阶段。
- 每个代码阶段完成后回归 UART DUMP、FreeRTOS/IWDG 行为和 Stage 10 稳定性基线。

## 8. 后续代码阶段边界

| 阶段 | 允许验证内容 | 明确不包含 |
| --- | --- | --- |
| Stage 11A | 文档、冲突分析、路线与风险 | 任何代码或工程配置修改 |
| Stage 11B | SDIO 和 SD 卡独立初始化、卡信息、基础读写 | 摄像头帧保存、DCMI/SDIO 动态切换 |
| Stage 11C | FATFS 挂载、测试文件写入与读回、CLI 查询 | 图像保存和实时采集写卡 |
| Stage 11D | 单帧缓冲区保存、DCMI/SDIO 互斥切换与恢复 | 连续录像、DCMI 与 SDIO 并行 |

## 9. Stage 11A 完成标准

1. 明确记录 SDIO 与 DCMI 在 PC8、PC9、PC11 上的引脚冲突。
2. 明确不做 DCMI 并行采集和 SDIO 写卡。
3. 明确采用互斥、分时的拍照式保存。
4. 明确 Stage 11B、Stage 11C、Stage 11D 的分阶段路线及边界。
5. 明确正点原子 SD 卡实验中可借鉴的初始化和验证流程。
6. 明确后续 CLI 命令规划及命令含义。
7. 明确风险列表和保护原则。
8. 本轮只新增本预研文档，不修改任何代码。

## Stage 11B-1 SD卡模块框架和CLI入口

本阶段已经新增 `camera_sd_storage` 模块，并提供纯软件状态结构、状态查询接口和受控的初始化请求入口。模块不使用动态内存，不访问文件系统，也不访问 SDIO 寄存器。

CLI 新增以下文本命令：

- `SD STATUS`：输出 SD 卡模块的软件状态、初始化请求计数、最近返回码及处理时间。
- `SD INIT`：记录一次初始化请求，提示必须先执行 SDIO 接管，并输出当前状态。

本阶段的 `SD INIT` 不会真正初始化 SDIO，固定返回 `CAMERA_SD_ERR_NEED_TAKEOVER`。该返回结果用于明确 PC8、PC9、PC11 与 DCMI 的复用冲突必须先通过受控接管流程解决。本阶段不切换 PC8、PC9、PC11，不调用 `HAL_SD_Init`，不挂载 FATFS，也不读写 SD 卡。

后续 Stage 11B-2 再单独实现并验证以下流程：

1. 停止 DCMI。
2. 停止相关 DMA，并确认传输已经结束。
3. 释放 PC8、PC9、PC11，进入 SDIO 接管模式。
4. 以不超过 400 kHz 的初始化时钟初始化 SDIO 和 SD 卡。
5. 初始化成功后提高 SDIO 时钟并读取 SD 卡信息。

Stage 11B-2 开始前仍需评审 DCMI/DMA 停止、GPIO 复用切换、失败回滚和 DCMI 恢复边界，不能把真实 SDIO 初始化直接加入当前稳定采集路径。

### Stage 11B-1 板测结果

#### 1. 启动情况

- 启动正常。
- 启动日志显示 `reset: iwdg=0`。
- 未出现 `FATAL`。
- 未出现反复复位。
- 未出现 IWDG 复位循环。

#### 2. HELP 测试

`HELP` 输出中可以看到新增的 `SD STATUS` 和 `SD INIT` 命令，帮助文本如下：

```text
SD STATUS - show SD storage status
SD INIT - request SD card init, currently deferred until SDIO takeover
```

#### 3. 首次 SD STATUS

首次执行 `SD STATUS` 时，模块处于未初始化状态，尚未收到初始化请求，输出如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=0
init_success_count=0
init_error_count=0
last_error_code=0
last_error_text=OK
```

结果符合 Stage 11B-1 设计：SDIO 和 FATFS 均未启用，同时由于 PC8、PC9、PC11 与 DCMI 冲突，`takeover_required` 保持为 1。

#### 4. SD INIT 返回

执行 `SD INIT` 后，CLI 正确提示需要先完成 SDIO 接管：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

该结果表明命令只进入受控软件入口，没有执行真实 SDIO 初始化。

#### 5. SD INIT 后的 SD STATUS

执行一次 `SD INIT` 后，状态输出如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
```

`init_attempt_count` 从 0 墕加到 1，最近返回码更新为 `CAMERA_SD_ERR_NEED_TAKEOVER`；初始化成功次数和硬件错误次数仍为 0，符合“记录请求但不操作硬件”的预期。

#### 6. 原有功能回归

| 测试项 | 结果 | 说明 |
| --- | --- | --- |
| basic | PASS | 基础功能正常。 |
| pc_dump | PASS | 图像导出正常，图像质量无警告。 |
| repeat | 20/20 PASS | 20 次请求全部通过，`frame_id` 连续。 |

回归结果说明新增模块框架和 CLI 入口未破坏现有 UART DUMP 与二进制图像请求路径。

#### 7. 最终 STATUS 关键状态

本次板测已单独执行最终 `STATUS`。以下按稳定性验证重点整理 `HOOK`、`IWDG` 和 `UART RX DMA` 三组结果。

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

未记录 Hook 故障或断言失败。

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=488
iwdg_refresh_skip_count=0
iwdg_last_refresh_ms=550609
iwdg_last_skip_ms=0
iwdg_last_skip_reason=0
iwdg_timeout_ms=8000
iwdg_camera_age_limit_ms=6000
iwdg_monitor_age_limit_ms=3000
iwdg_test_mode=0
```

IWDG 已启用并正常刷新，没有跳过喂狗，未进入看门狗测试模式。

`UART RX DMA`：

```text
uart_dma_event_count=32
uart_dma_rx_bytes=344
stream_buffer_write_bytes=344
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

UART DMA 接收字节数与 StreamBuffer 写入字节数一致，没有缓冲区溢出、UART DMA 错误、恢复或重同步事件。

#### 8. 板测结论

Stage 11B-1 验证通过。新增 `camera_sd_storage` 模块和 `SD STATUS`、`SD INIT` CLI 入口后，系统启动正常；`SD INIT` 能正确提示 `NEED_TAKEOVER`，说明当前没有真正启用 SDIO、没有切换 PC8、PC9、PC11，也没有接入 FATFS。

`basic`、`pc_dump` 和 `repeat` 回归均通过，最终状态中 Hook、IWDG 和 UART RX DMA 指标正常，说明该框架没有破坏现有 UART DUMP、二进制请求、FreeRTOS 和 IWDG 正常路径。

## Stage 11B-2 SDIO接管模式预留接口

### 1. 本轮目的

Stage 11B-2 在现有 `camera_sd_storage` 软件框架上增加 SDIO 接管模式状态和 CLI 预留入口，为后续停止 DCMI、释放冲突引脚并切换到 SDIO 做接口准备。

本轮新增：

- SDIO 接管状态及进入、退出请求的统计字段。
- `SD TAKEOVER STATUS` 状态查询命令。
- `SD TAKEOVER ENTER` 进入接管请求命令。
- `SD TAKEOVER EXIT` 退出接管请求命令。
- 在原有 `SD STATUS` 末尾输出全部接管状态字段。

### 2. 本轮明确不做

- 不停止 DCMI。
- 不停止 DCMI DMA。
- 不释放或切换 PC8、PC9、PC11。
- 不初始化 SDIO，也不调用任何 SD 卡 HAL 读写接口。
- 不接入或挂载 FATFS。
- 不读写 SD 卡。

所有接管操作仍是纯软件请求，不会改变当前稳定的摄像头采集链路。

### 3. 接管状态设计

| 状态 | 数值 | 含义 |
| --- | ---: | --- |
| `IDLE` | 0 | 尚未收到接管进入或退出请求。 |
| `ENTER_DEFERRED` | 1 | 已请求进入接管模式，但停止 DCMI 和切换引脚尚未实现。 |
| `ACTIVE` | 2 | 已真正进入 SDIO 接管模式；本轮禁止设置为该状态。 |
| `EXIT_DEFERRED` | 3 | 已请求退出接管模式，但恢复 DCMI 尚未实现。 |
| `ERROR` | 4 | 真实接管流程发生错误时使用；本轮 deferred 请求不视为硬件错误。 |

初始状态为 `IDLE`。执行 `SD TAKEOVER ENTER` 后状态变为 `ENTER_DEFERRED`；执行 `SD TAKEOVER EXIT` 后状态变为 `EXIT_DEFERRED`。由于本轮没有真实接管 SDIO，任何入口都不会把状态设为 `ACTIVE`，`takeover_enter_success_count` 和 `takeover_exit_success_count` 保持为 0。

### 4. 命令设计

| 命令 | 当前行为 |
| --- | --- |
| `SD TAKEOVER STATUS` | 输出接管状态、状态文本、进入和退出计数、错误码及最近操作时间。 |
| `SD TAKEOVER ENTER` | 记录一次进入请求，返回 `TAKEOVER_NOT_IMPLEMENTED`，状态置为 `ENTER_DEFERRED`。 |
| `SD TAKEOVER EXIT` | 记录一次退出请求，返回 `TAKEOVER_NOT_IMPLEMENTED`，状态置为 `EXIT_DEFERRED`。 |

`SD TAKEOVER ENTER` 和 `SD TAKEOVER EXIT` 返回 deferred 后都会输出当前接管状态，便于确认请求计数和状态变化。`TAKEOVER_NOT_IMPLEMENTED` 表示硬件接管尚未实现，不计入 SD 卡硬件失败，因此本轮 `takeover_error_count` 保持为 0。

### 5. 当前行为边界

- `SD TAKEOVER ENTER` 当前返回 `CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED`。
- `SD TAKEOVER EXIT` 当前返回 `CAMERA_SD_ERR_TAKEOVER_NOT_IMPLEMENTED`。
- 接管状态不会被设置为 `ACTIVE`。
- 原有 `SD INIT` 行为保持不变，仍返回 `CAMERA_SD_ERR_NEED_TAKEOVER`，不会真正初始化 SDIO。
- 原有 `SD STATUS` 字段全部保留，仅在末尾增加接管状态字段。
- 原有 `STATUS`、`DUMP`、二进制请求和 `IWDGTEST` 路径不变。

### 6. 后续 Stage 11B-3 计划

Stage 11B-3 先验证相机停止和恢复的接口边界，暂时仍不实现真实 SDIO 初始化：

1. 增加相机停止接口边界。
2. 增加 DCMI DMA 停止状态确认。
3. 增加 PC8、PC9、PC11 冲突引脚释放前的条件检查。
4. 验证停止失败时的状态恢复和错误回滚。
5. 验证恢复 DCMI/DMA 后现有采集链路仍能正常工作。

### 7. Stage 11B-2 板测计划

本轮不由 Codex 执行硬件测试。固件烧录后按以下顺序验证：

1. 确认系统启动正常，无反复复位或 IWDG 复位循环。
2. 确认 `HELP` 中出现 `SD TAKEOVER STATUS`、`SD TAKEOVER ENTER` 和 `SD TAKEOVER EXIT`。
3. 确认 `SD STATUS` 保留原字段并输出新增接管字段。
4. 执行 `SD TAKEOVER STATUS`，确认初始状态为 `IDLE`。
5. 执行 `SD TAKEOVER ENTER`，确认提示 deferred、返回 `TAKEOVER_NOT_IMPLEMENTED` 且状态为 `ENTER_DEFERRED`。
6. 执行 `SD TAKEOVER EXIT`，确认提示 deferred、返回 `TAKEOVER_NOT_IMPLEMENTED` 且状态为 `EXIT_DEFERRED`。
7. 执行 `SD INIT`，确认仍提示 `NEED_TAKEOVER`。
8. 回归 `basic`、`pc_dump` 和 `repeat 20/20`。

### 8. Stage 11B-2 板测结果

#### 8.1 启动情况

- 系统启动正常。
- 启动日志显示 `reset: iwdg=0`。
- 未出现 `FATAL`。
- 未出现反复复位。
- 未出现 IWDG 复位循环。

#### 8.2 HELP 测试

`HELP` 输出中可以看到以下全部 SD 卡相关命令：

```text
SD STATUS
SD INIT
SD TAKEOVER STATUS
SD TAKEOVER ENTER
SD TAKEOVER EXIT
```

HELP 测试通过，Stage 11B-1 原有命令和 Stage 11B-2 新增命令均保留。

#### 8.3 初始 SD STATUS 接管字段

系统启动后首次执行 `SD STATUS`，接管字段如下：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=0
takeover_exit_attempt_count=0
takeover_enter_success_count=0
takeover_exit_success_count=0
takeover_error_count=0
last_takeover_error_code=0
last_takeover_error_text=OK
last_takeover_operation_ms=0
```

初始状态为 `IDLE`，所有进入、退出、成功和错误计数均为 0，符合软件状态初始化设计。

#### 8.4 SD TAKEOVER STATUS 初始结果

单独执行 `SD TAKEOVER STATUS`，得到：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=0
takeover_exit_attempt_count=0
takeover_enter_success_count=0
takeover_exit_success_count=0
takeover_error_count=0
last_takeover_error_code=0
last_takeover_error_text=OK
```

专用状态命令与 `SD STATUS` 中的接管字段一致。

#### 8.5 SD TAKEOVER ENTER

执行 `SD TAKEOVER ENTER`，CLI 返回：

```text
SD TAKEOVER ENTER: deferred, DCMI stop and PC8/PC9/PC11 switch are not implemented yet.
```

随后查询 `SD TAKEOVER STATUS`：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=1
takeover_exit_attempt_count=0
takeover_enter_success_count=0
takeover_exit_success_count=0
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

进入请求次数正确增加到 1，状态进入 `ENTER_DEFERRED`，但成功计数仍为 0，未误报真实硬件接管成功。`TAKEOVER_NOT_IMPLEMENTED` 是本阶段预期的 deferred 返回，不计入硬件错误。

#### 8.6 SD TAKEOVER EXIT

执行 `SD TAKEOVER EXIT`，CLI 返回：

```text
SD TAKEOVER EXIT: deferred, DCMI restore is not implemented yet.
```

随后查询 `SD TAKEOVER STATUS`：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=1
takeover_exit_attempt_count=1
takeover_enter_success_count=0
takeover_exit_success_count=0
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

退出请求次数正确增加到 1，状态进入 `EXIT_DEFERRED`，没有进入 `ACTIVE`，也没有误报硬件失败。

#### 8.7 SD INIT 行为保持验证

执行 `SD INIT` 后仍返回 `NEED_TAKEOVER`。该命令没有真正初始化 SDIO、没有切换 PC8、PC9、PC11，也没有接入 FATFS。

`SD INIT` 后的 SD 状态如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
takeover_state=3
takeover_state_text=EXIT_DEFERRED
```

`SD INIT` 没有改变接管状态，`takeover_state` 仍保持 `EXIT_DEFERRED`，符合两个软件入口相互独立且均不执行硬件操作的设计。

板测中观察到 `last_operation_ms=1245613`，该数值更像系统 tick 时间戳，而不是本次命令的实际处理耗时。这个观察项不影响 Stage 11B-2 接口验证结论，后续阶段应明确字段语义，并在需要时修正为真实命令耗时统计。

#### 8.8 原有功能回归

| 测试项 | 结果 | 说明 |
| --- | --- | --- |
| basic | PASS | 基础功能正常。 |
| pc_dump | PASS | 图像导出正常，图像质量无警告。 |
| repeat | 20/20 PASS | `frame_id` 从 3 到 22 连续，平均耗时约 3465.83 ms。 |

原有摄像头采集、PC DUMP 和二进制连续请求链路均未受到影响。

#### 8.9 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=22
dump_success_count=22
dump_error_count=0
binary_request_count=21
binary_request_success_count=21
binary_request_error_count=0
last_binary_request_seq=20
last_error_code=0
last_dump_time_ms=3515
```

22 次 DUMP 请求全部成功，21 次二进制请求全部成功，未记录请求错误。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7656
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

任务栈和堆仍有余量，当前记录中没有资源耗尽迹象。

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

未记录 Hook 故障或断言失败。

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=69
monitor_heartbeat_age_ms=619
```

相机服务任务和监控任务心跳均处于有效范围内。

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=1534
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

IWDG 已启用并持续正常刷新，没有跳过喂狗，也未进入测试模式。

`UART RX DMA`：

```text
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

未发生 StreamBuffer 溢出、UART DMA 错误、恢复或协议重同步事件。

### 9. Stage 11B-2 板测结论

Stage 11B-2 验证通过。新增 SDIO 接管模式预留接口后，`SD TAKEOVER STATUS`、`SD TAKEOVER ENTER` 和 `SD TAKEOVER EXIT` 命令均可正常工作；ENTER 和 EXIT 均只进入 deferred 状态，没有真正停止 DCMI、没有切换 PC8、PC9、PC11、没有初始化 SDIO，也没有接入 FATFS。

`SD INIT` 仍保持 `NEED_TAKEOVER` 行为。`basic`、`pc_dump` 和 `repeat` 回归通过，IWDG、Hook、心跳和 UART DMA 状态正常，说明本轮接管模式软件接口没有破坏现有摄像头采集、DUMP、二进制请求和运行保护机制。

## Stage 11B-3 相机停止/恢复接口边界设计

### 1. 本轮目的

Stage 11B-3 新增 `camera_snapshot_control` 模块，用纯软件状态描述拍照保存前后的相机控制边界，并增加以下 CLI 命令：

- `SNAPSHOT STATUS`
- `SNAPSHOT PREPARE`
- `SNAPSHOT RESTORE`

该模块为后续实现 SD 卡拍照保存前的相机停止、DCMI DMA 停止、稳定帧确认、冲突引脚释放，以及保存完成后的相机恢复流程提供独立接口边界。

### 2. 本轮明确不做

- 不停止 DCMI。
- 不停止或反初始化 DMA。
- 不释放或切换 PC8、PC9、PC11。
- 不初始化 SDIO。
- 不接入或挂载 FATFS。
- 不读写 SD 卡。
- 不保存 raw、BMP 或其他图片文件。

本轮所有接口只维护软件状态和请求计数，不改变当前 DCMI、DMA、GPIO、SDIO 或图像采集链路。

### 3. 状态设计

| 状态 | 数值 | 含义 |
| --- | ---: | --- |
| `IDLE` | 0 | 尚未收到相机准备或恢复请求。 |
| `PREPARE_DEFERRED` | 1 | 已收到准备请求，但停止相机、DMA 和释放引脚尚未实现。 |
| `CAMERA_PAUSED` | 2 | 相机和 DCMI DMA 已真正停止；本轮禁止设置为该状态。 |
| `RESTORE_DEFERRED` | 3 | 已收到恢复请求，但恢复引脚、DCMI 和 DMA 尚未实现。 |
| `ERROR` | 4 | 真实硬件控制流程发生错误时使用；本轮 deferred 请求不视为硬件错误。 |

初始状态为 `IDLE`。执行 `SNAPSHOT PREPARE` 后状态变为 `PREPARE_DEFERRED`；执行 `SNAPSHOT RESTORE` 后状态变为 `RESTORE_DEFERRED`。由于没有真正停止相机，本轮不会进入 `CAMERA_PAUSED`，准备和恢复成功计数均保持为 0。

### 4. 命令设计

| 命令 | 当前行为 |
| --- | --- |
| `SNAPSHOT STATUS` | 输出相机控制状态、请求计数、错误码、实际软件处理耗时和后续硬件操作需求标志。 |
| `SNAPSHOT PREPARE` | 记录一次准备请求，返回 `CAMERA_STOP_NOT_IMPLEMENTED`，状态置为 `PREPARE_DEFERRED`。 |
| `SNAPSHOT RESTORE` | 记录一次恢复请求，返回 `CAMERA_RESTORE_NOT_IMPLEMENTED`，状态置为 `RESTORE_DEFERRED`。 |

`SNAPSHOT PREPARE` 和 `SNAPSHOT RESTORE` 都会在返回 deferred 提示后输出当前状态。`last_operation_ms` 按函数出口 tick 减去函数入口 tick 计算，记录本次软件命令的真实处理耗时，不记录系统绝对 tick。

### 5. 当前行为边界

- `SNAPSHOT PREPARE` 当前返回 `CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED`。
- `SNAPSHOT RESTORE` 当前返回 `CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED`。
- deferred 返回不计为硬件故障，`control_error_count` 保持为 0。
- `frame_buffer_ready` 固定为 0，本轮不检查真实帧缓冲区。
- 相机控制状态不会被设置为 `CAMERA_PAUSED`。
- 当前 DCMI、DMA、GPIO 和图像采集链路不会发生变化。
- 原有 `SD STATUS`、`SD INIT`、SD TAKEOVER、`STATUS`、`DUMP`、二进制请求和 `IWDGTEST` 行为保持不变。

### 6. 与 SD TAKEOVER 的关系

后续真正的 SD 卡单帧保存流程应按以下顺序组织：

1. `SNAPSHOT PREPARE`：采集并固定一帧，停止 DCMI 和 DMA，释放冲突引脚。
2. `SD TAKEOVER ENTER`：将 PC8、PC9、PC11 等相关引脚切换给 SDIO，并初始化 SDIO。
3. `SD INIT / FATFS`：初始化 SD 卡并挂载文件系统。
4. `SNAPSHOT SD`：将稳定帧缓冲区写入图像文件。
5. `SD TAKEOVER EXIT`：退出 SDIO 接管并释放 SDIO 对冲突引脚的占用。
6. `SNAPSHOT RESTORE`：恢复 DCMI 引脚、DMA 和相机采集。

这些步骤必须按状态机串行执行，并为每一步定义失败回滚路径；不能在 DCMI 连续采集期间同时让 SDIO 使用冲突引脚。

### 7. 后续 Stage 11B-4 计划

Stage 11B-4 继续进行相机停止边界预研，暂时仍不初始化 SDIO：

1. 检查现有相机采集启动和停止相关函数。
2. 找出 DCMI 与 DMA 句柄的定义位置、所有权和调用上下文。
3. 明确真正停止 DCMI 的最小改动点及帧边界条件。
4. 明确 DMA 停止确认、帧缓冲区所有权和失败回滚顺序。
5. 明确恢复 DCMI/DMA 后的回归验证项目。

### 8. Stage 11B-3 板测计划

本轮不由 Codex 执行硬件测试。固件烧录后按以下顺序验证：

1. 确认系统启动正常，无反复复位或 IWDG 复位循环。
2. 确认 `HELP` 中出现 `SNAPSHOT STATUS`、`SNAPSHOT PREPARE` 和 `SNAPSHOT RESTORE`。
3. 执行 `SNAPSHOT STATUS`，确认初始状态为 `IDLE`，固定需求标志为 1，`frame_buffer_ready` 为 0。
4. 执行 `SNAPSHOT PREPARE`，确认提示 deferred、返回 `CAMERA_STOP_NOT_IMPLEMENTED`，状态为 `PREPARE_DEFERRED`。
5. 执行 `SNAPSHOT RESTORE`，确认提示 deferred、返回 `CAMERA_RESTORE_NOT_IMPLEMENTED`，状态为 `RESTORE_DEFERRED`。
6. 确认 `last_operation_ms` 是短命令处理耗时，而不是系统绝对 tick。
7. 确认 `SD STATUS`、SD TAKEOVER 和 `SD INIT` 行为保持正常。
8. 回归 `basic`、`pc_dump` 和 `repeat 20/20`。
9. 检查 IWDG 未跳过喂狗、Hook 未触发、UART DMA 无错误或溢出。

### 9. Stage 11B-3 板测结果

#### 9.1 启动情况

- 系统启动正常。
- 启动日志显示 `reset: iwdg=0`。
- 未出现 `FATAL`。
- 未出现反复复位。
- 未出现 IWDG 复位循环。

#### 9.2 HELP 测试

`HELP` 输出中可以看到 Stage 11B-3 新增命令及其完整说明：

```text
SNAPSHOT STATUS - show snapshot camera control status
SNAPSHOT PREPARE - request camera stop boundary before SD save, currently deferred
SNAPSHOT RESTORE - request camera restore boundary after SD save, currently deferred
```

同时确认以下原有命令仍然存在：

```text
SD STATUS
SD INIT
SD TAKEOVER STATUS
SD TAKEOVER ENTER
SD TAKEOVER EXIT
DUMP
IWDGTEST CAMERA_TIMEOUT
```

HELP 测试通过，新增命令未覆盖或删除原有命令入口。

#### 9.3 首次 SNAPSHOT STATUS

系统启动后首次执行 `SNAPSHOT STATUS`，输出如下：

```text
camera_control_state=0
camera_control_state_text=IDLE
prepare_attempt_count=0
restore_attempt_count=0
prepare_success_count=0
restore_success_count=0
control_error_count=0
last_error_code=0
last_error_text=OK
last_operation_ms=0
dcmi_stop_required=1
dcmi_dma_stop_required=1
conflict_pin_release_required=1
camera_restore_required=1
frame_buffer_required=1
frame_buffer_ready=0
```

初始状态为 `IDLE`，所有请求、成功和错误计数均为 0；DCMI 停止、DMA 停止、冲突引脚释放、相机恢复和帧缓冲区需求标志均为 1。由于本轮没有检查真实帧缓冲区，`frame_buffer_ready` 保持为 0。

#### 9.4 SNAPSHOT PREPARE

执行 `SNAPSHOT PREPARE`，CLI 返回：

```text
SNAPSHOT PREPARE: deferred, DCMI stop, DMA stop and PC8/PC9/PC11 release are not implemented yet.
```

随后查询 `SNAPSHOT STATUS`：

```text
camera_control_state=1
camera_control_state_text=PREPARE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=0
prepare_success_count=0
restore_success_count=0
control_error_count=0
last_error_code=2
last_error_text=CAMERA_STOP_NOT_IMPLEMENTED
last_operation_ms=0
dcmi_stop_required=1
dcmi_dma_stop_required=1
conflict_pin_release_required=1
camera_restore_required=1
frame_buffer_required=1
frame_buffer_ready=0
```

准备请求次数正确增加到 1，状态进入 `PREPARE_DEFERRED`。准备成功次数仍为 0，说明没有误报相机已停止；返回的 `CAMERA_STOP_NOT_IMPLEMENTED` 是本阶段预期的 deferred 结果，不计入硬件错误。

`last_operation_ms=0` 符合短软件路径在 1 ms tick 分辨率内完成的预期，也说明该字段记录的是入口到出口的耗时差值，而不是系统绝对 tick。

#### 9.5 SNAPSHOT RESTORE

执行 `SNAPSHOT RESTORE`，CLI 返回：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

随后查询 `SNAPSHOT STATUS`：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=0
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
last_operation_ms=0
dcmi_stop_required=1
dcmi_dma_stop_required=1
conflict_pin_release_required=1
camera_restore_required=1
frame_buffer_required=1
frame_buffer_ready=0
```

恢复请求次数正确增加到 1，状态进入 `RESTORE_DEFERRED`。恢复成功次数仍为 0，相机控制状态没有进入 `CAMERA_PAUSED`，也没有误报硬件故障。

#### 9.6 SD 命令回归

首次执行 `SD STATUS`，核心状态和接管状态均正常：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=0
init_success_count=0
init_error_count=0
last_error_code=0
last_error_text=OK
takeover_state=0
takeover_state_text=IDLE
```

执行 `SD TAKEOVER STATUS`，输出如下：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=0
takeover_exit_attempt_count=0
takeover_enter_success_count=0
takeover_exit_success_count=0
takeover_error_count=0
last_takeover_error_code=0
last_takeover_error_text=OK
```

执行 `SD INIT`，仍返回原有提示：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

`SD INIT` 后状态如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
```

上述结果确认 Stage 11B-3 没有改变 `SD STATUS`、SD TAKEOVER 或 `SD INIT` 的既有行为，也没有真正初始化 SDIO、切换冲突引脚或接入 FATFS。

板测中观察到 `SD INIT` 后 `last_operation_ms=259963`，该数值仍更像系统 tick 时间戳，而不是本次命令的实际处理耗时。这是 `camera_sd_storage` 模块的既有统计语义问题，不影响 Stage 11B-3 相机控制边界验证结论；后续阶段应将其修正为函数出口 tick 减函数入口 tick 的真实命令耗时。

#### 9.7 原有图像功能回归

| 测试项 | 结果 | 说明 |
| --- | --- | --- |
| basic | PASS | 基础功能正常。 |
| pc_dump | PASS | 图像质量无警告，`frame_id=2`。 |
| repeat | 20/20 PASS | `frame_id` 从 3 到 22 连续。 |

repeat 性能统计：

- 平均耗时：3463.43 ms。
- 最短耗时：3433.29 ms。
- 最长耗时：3468.43 ms。

摄像头采集、PC DUMP 和二进制连续请求链路均未受到新增软件边界接口影响。

#### 9.8 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=22
dump_success_count=22
dump_error_count=0
binary_request_count=21
binary_request_success_count=21
binary_request_error_count=0
last_binary_request_seq=20
last_error_code=0
last_dump_time_ms=3503
```

22 次 DUMP 请求全部成功，21 次二进制请求全部成功，没有记录请求错误。

`HEALTH`：

```text
health_sample_count=715
camera_service_stack_min_free_bytes=7656
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

任务栈和堆仍有余量，当前记录中没有资源耗尽迹象。

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

未记录 Hook 故障或断言失败。

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=72
monitor_heartbeat_age_ms=171
```

相机服务任务和监控任务心跳均处于有效范围内。

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=715
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

IWDG 已启用并正常刷新，没有跳过喂狗，也未进入测试模式。

`UART RX DMA`：

```text
uart_dma_event_count=38
uart_dma_rx_bytes=440
stream_buffer_write_bytes=440
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

UART DMA 接收字节数与 StreamBuffer 写入字节数一致，没有缓冲区溢出、UART DMA 错误、恢复或协议重同步事件。

### 10. Stage 11B-3 板测结论

Stage 11B-3 验证通过。新增 `camera_snapshot_control` 模块和 `SNAPSHOT STATUS`、`SNAPSHOT PREPARE`、`SNAPSHOT RESTORE` 命令后，系统启动正常；PREPARE 和 RESTORE 均只进入 deferred 状态，没有真正停止 DCMI、没有停止 DMA、没有释放 PC8、PC9、PC11、没有初始化 SDIO，也没有接入 FATFS。

`SD STATUS`、`SD TAKEOVER STATUS` 和 `SD INIT` 行为保持正常。`basic`、`pc_dump` 和 `repeat` 回归通过，IWDG、Hook、心跳和 UART DMA 状态正常，说明本轮相机停止/恢复接口边界没有破坏现有摄像头采集、DUMP、二进制请求和运行保护机制。

## Stage 11B-4 真实相机停止/恢复最小改动点梳理

本轮新增 `STAGE11_CAMERA_STOP_RESTORE_ANALYSIS.md`，只进行源码搜索、采集链路分析和后续接口设计，不修改任何 C/H 源码或工程配置。

分析已经定位后续真实 `SNAPSHOT PREPARE` / `SNAPSHOT RESTORE` 的最小改动点：

- 在 `camera_rtos` 中统一门控文本 DUMP、二进制请求和 SNAPSHOT 状态，并拆分“捕获/处理”与“UART 发送”。
- 在 `camera_dcmi_dma` 中补充可返回结果的 DCMI/DMA 停止确认和冲突引脚释放/恢复边界。
- 在 `camera_frame_buffer` 中补充 front 有效帧状态，避免仅凭非空地址判断帧可保存。
- 保持 SDIO 接管必须晚于相机暂停、DMA 停止和有效 front 确认。

源码表明当前 `CAMERA_MODE_PC_DUMP_RGB565` 是按请求启动单帧采集，采集完成后已经停止 DCMI/DMA；因此下一步建议进入 Stage 11B-5，先实现相机停止/恢复的软件状态保护、DUMP 禁止重入和捕获/发送边界拆分，仍不切换 PC8、PC9、PC11，也不初始化 SDIO。

## Stage 11B-5 SNAPSHOT软件状态保护

### 1. 本轮目的

Stage 11B-5 在现有 `camera_snapshot_control` 状态机上增加纯软件保护：

- `SNAPSHOT PREPARE` 后激活软件保护。
- 软件保护期间阻止文本 DUMP 和合法二进制图像请求进入采集及 OV56RGB5 发送流程。
- `SNAPSHOT RESTORE` 后退出软件保护，使文本和二进制图像请求恢复正常。
- 为后续真实停止 DCMI、冻结 front buffer 和保存到 SD 卡准备统一门控状态。

### 2. 本轮明确不做

- 不停止 DCMI。
- 不停止、反初始化或重启 DMA。
- 不释放或切换 PC8、PC9、PC11。
- 不初始化 SDIO。
- 不接入或挂载 FATFS。
- 不读写 SD 卡或图片文件。

本轮不会修改 DCMI、DMA、GPIO、SDIO、FATFS、UART DMA 或图像协议实现。

### 3. 软件保护状态字段

| 字段 | 含义 |
| --- | --- |
| `software_guard_active` | SNAPSHOT 软件保护激活标志；PREPARE 置 1，RESTORE 清 0。 |
| `dump_block_required` | 当前是否需要阻止图像导出；与 guard 状态同步。 |
| `dump_block_count` | 软件保护状态下被阻止的文本 DUMP 次数。 |
| `binary_block_count` | 软件保护状态下被阻止的合法二进制图像请求次数。 |

这些字段追加到 `SNAPSHOT STATUS`，原有状态字段和字段名保持不变。

### 4. 当前保护行为

`SNAPSHOT PREPARE` 仍返回 `CAMERA_SNAPSHOT_ERR_CAMERA_STOP_NOT_IMPLEMENTED` 并保持 `PREPARE_DEFERRED`，但会将 `software_guard_active` 和 `dump_block_required` 置为 1。该状态只表示禁止新的图像导出请求，不表示相机已经进入 `CAMERA_PAUSED`。

`SNAPSHOT RESTORE` 仍返回 `CAMERA_SNAPSHOT_ERR_CAMERA_RESTORE_NOT_IMPLEMENTED` 并保持 `RESTORE_DEFERRED`，同时将 `software_guard_active` 和 `dump_block_required` 清零，使后续 DUMP 恢复正常。

文本 DUMP 和二进制图像请求最终共用 `camera_rtos.c` 内的 `Camera_RTOS_ProcessDumpRequest()`。软件保护判断位于该统一入口的最前面，因此被阻止的请求不会启动 DCMI 快照，也不会发送 OV56RGB5 图像帧：

- 文本 DUMP 输出 `DUMP blocked: snapshot software guard active.`，并增加 `dump_block_count`。
- 合法二进制请求不输出文本或图像帧，并增加 `binary_block_count`；PC 端等待超时属于预期现象。
- 两类阻止都会增加 DUMP 错误统计，并将 RTOS 最近错误码设为 SNAPSHOT guard active。
- 合法二进制请求被阻止不属于协议格式错误，不增加 CRC、版本、类型、长度或帧尾错误计数。
- 阻止路径不增加 UART 错误计数。

### 5. 板测验证方法

1. 启动后执行 `SNAPSHOT STATUS`，确认 `software_guard_active=0`、两个 block 计数为 0。
2. 执行 `basic`，确认正常通过。
3. 执行 `SNAPSHOT PREPARE`，确认 deferred 提示保持不变。
4. 再次执行 `SNAPSHOT STATUS`，确认 `software_guard_active=1`、`dump_block_required=1`。
5. 发送文本 `DUMP`，确认只收到 blocked 提示且 `dump_block_count` 增加。
6. 发送合法二进制图像请求，确认没有 OV56RGB5 帧、PC 端超时且 `binary_block_count` 增加。
7. 检查协议错误分类计数和 UART DMA 错误计数未因 guard 阻止而增加。
8. 执行 `SNAPSHOT RESTORE`，确认 `software_guard_active=0`、`dump_block_required=0`。
9. 重新执行 `basic`、`pc_dump` 和 `repeat 20/20`，确认全部恢复 PASS。
10. 检查 IWDG 未跳过喂狗、Hook 未触发、UART DMA 无错误或溢出。

### 6. 后续 Stage 11B-6 建议

Stage 11B-6 可在现有软件保护基础上尝试真实 DCMI 停止，但暂时仍不切换 SDIO 引脚：

1. PREPARE 激活 guard 后捕获并固定一帧。
2. 尝试调用经过确认和封装的 DCMI 停止接口。
3. 确认 DMA 已停止且 front buffer 为完整 160×120 RGB565 帧。
4. 验证停止采集后 front buffer 仍可稳定读取或保存。
5. RESTORE 后验证下一次单帧 DUMP 能重新配置 DMA 并正常采集。
6. 继续执行 basic、pc_dump、repeat、IWDG、Hook 和 UART DMA 回归。

### 7. Stage 11B-5 板测结果

#### 7.1 启动与初始状态

系统启动正常，启动信息为 `reset: iwdg=0`；测试期间无 FATAL、无反复复位、无 IWDG 复位循环。

初始 `SNAPSHOT STATUS`：

```text
camera_control_state=0
camera_control_state_text=IDLE
software_guard_active=0
dump_block_required=0
dump_block_count=0
binary_block_count=0
```

激活 guard 前执行 `basic`，结果 PASS，`frame_id=1`，CRC 校验一致。

#### 7.2 SNAPSHOT PREPARE 与软件保护验证

执行 `SNAPSHOT PREPARE` 后：

```text
camera_control_state=1
camera_control_state_text=PREPARE_DEFERRED
prepare_attempt_count=1
software_guard_active=1
dump_block_required=1
dump_block_count=0
binary_block_count=0
last_error_code=2
last_error_text=CAMERA_STOP_NOT_IMPLEMENTED
```

guard 状态下执行文本 `DUMP`：

- 输出 `DUMP blocked: snapshot software guard active.`。
- 未发送 OV56RGB5 图像帧。
- `dump_block_count` 从 0 增加到 1。

guard 状态下执行二进制 `basic`：

- 响应长度为 0 B，PC 端接收超时，只收到 0/38426 B，测试结果为 FAIL。
- 该 FAIL 是本轮软件保护生效后的预期现象。
- `binary_block_count` 从 0 增加到 1。
- 未增加协议解析错误。

#### 7.3 SNAPSHOT RESTORE 与图像功能回归

执行 `SNAPSHOT RESTORE` 后：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
```

软件保护解除后的图像功能回归结果：

- `basic`：PASS，`frame_id=2`。
- `pc_dump`：PASS，`frame_id=3`，图像质量无警告。
- `repeat`：20/20 PASS，`frame_id` 从 4 到 23 连续。
- `repeat` 平均耗时 3465.61 ms，最短耗时 3447.75 ms，最长耗时 3467.87 ms。

#### 7.4 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=25
dump_success_count=23
dump_error_count=2
binary_request_count=23
binary_request_success_count=22
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
last_dump_time_ms=3518
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和二进制图像请求各被阻止一次。`binary_request_error_count=0` 是正确结果：二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于本轮预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7672
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=32
monitor_heartbeat_age_ms=663
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=483
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=44
uart_dma_rx_bytes=513
stream_buffer_write_bytes=513
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 8. Stage 11B-5 板测结论

Stage 11B-5 验证通过。`SNAPSHOT PREPARE` 后软件保护状态生效，文本 DUMP 被阻止且未发送 OV56RGB5 图像帧，二进制图像请求被阻止并表现为 PC 端接收超时；`SNAPSHOT RESTORE` 后软件保护解除，basic、pc_dump、repeat 全部恢复正常。

最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明本轮软件状态保护没有破坏现有摄像头采集、DUMP、二进制请求和运行保护机制。

后续 Stage 11B-6 建议在软件保护基础上尝试真实 `HAL_DCMI_Stop`，暂时仍不切换 PC8、PC9、PC11，也不初始化 SDIO；先验证停止采集后系统不会死锁，并确认 RESTORE 后图像导出能够恢复。

## Stage 11B-6 真实 HAL_DCMI_Stop 最小验证

### 1. 本轮目的

- 在 `SNAPSHOT PREPARE` 中先激活软件保护，再真实调用一次 `HAL_DCMI_Stop(&g_camera_dcmi)`。
- 记录 HAL 返回状态，验证停止 DCMI 不会导致系统死机、复位或 IWDG 异常。
- 验证软件保护状态下文本 DUMP 和二进制图像请求仍被阻止。
- 验证 `SNAPSHOT RESTORE` 清除 guard 后，现有图像导出链路是否能够自行恢复。

项目现有 DCMI 句柄为 `g_camera_dcmi`，定义在 `camera_dcmi_dma.c`，并已通过 `camera_dcmi_dma.h` 导出。本轮直接使用该公开声明，不移动句柄、不修改 Core，也不重构 DCMI 初始化代码。

### 2. 本轮明确不做

- 不调用 `HAL_DCMI_Start_DMA`。
- 不直接调用 `HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不释放或切换 PC8、PC9、PC11。
- 不初始化 SDIO，不出现 `GPIO_AF12_SDIO`。
- 不接入 FATFS，不读写 SD 卡或图片文件。
- 不修改 UART DMA、二进制请求帧格式、OV56RGB5 图像帧格式、IWDG、FreeRTOS 任务优先级或任务栈大小。

本轮唯一新增的真实硬件动作是 `HAL_DCMI_Stop(&g_camera_dcmi)`，且只位于 `Camera_SnapshotControl_RequestPrepare()` 中。

### 3. 新增状态字段

| 字段 | 含义 |
| --- | --- |
| `real_dcmi_stop_enabled` | 是否启用真实 `HAL_DCMI_Stop` 验证，本轮固定为 1。 |
| `dcmi_stop_attempt_count` | `HAL_DCMI_Stop` 调用次数。 |
| `dcmi_stop_success_count` | `HAL_DCMI_Stop` 返回 `HAL_OK` 的次数。 |
| `dcmi_stop_error_count` | `HAL_DCMI_Stop` 返回非 `HAL_OK` 的次数。 |
| `last_dcmi_stop_hal_status` | 最近一次 `HAL_DCMI_Stop` 的 HAL 返回值。 |

上述字段追加到 `SNAPSHOT STATUS`，原有字段名以及 `software_guard_active`、`dump_block_required`、`dump_block_count`、`binary_block_count` 均保持不变。新增错误码 `CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED`，CLI 文本为 `DCMI_STOP_FAILED`。

### 4. SNAPSHOT PREPARE 状态处理

执行 `SNAPSHOT PREPARE` 时先记录入口 tick、增加 `prepare_attempt_count`，并将 `software_guard_active` 和 `dump_block_required` 置 1；随后增加 `dcmi_stop_attempt_count`，调用 `HAL_DCMI_Stop(&g_camera_dcmi)` 并记录 `last_dcmi_stop_hal_status`。`last_operation_ms` 按出口 tick 减入口 tick 计算。

如果返回 `HAL_OK`：

- 增加 `dcmi_stop_success_count` 和 `prepare_success_count`。
- 将 `camera_control_state` 设置为 `CAMERA_PAUSED`。
- 将 `last_error_code` 设置为 `CAMERA_SNAPSHOT_OK` 并返回该结果。
- CLI 输出 `SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.`。

如果返回非 `HAL_OK`：

- 增加 `dcmi_stop_error_count` 和 `control_error_count`。
- 将 `camera_control_state` 设置为 `ERROR`。这是一次已经执行但失败的真实硬件动作，因此不再使用仅表示功能延后的 `PREPARE_DEFERRED`。
- 将 `last_error_code` 设置为 `CAMERA_SNAPSHOT_ERR_DCMI_STOP_FAILED` 并返回该结果。
- 保持 `software_guard_active=1` 和 `dump_block_required=1`，不触发复位，也不执行恢复动作。
- CLI 输出 `SNAPSHOT PREPARE: DCMI stop failed, snapshot software guard remains active.`。

### 5. 当前 RESTORE 策略

`SNAPSHOT RESTORE` 保持最小化：只增加 `restore_attempt_count`，清除 `software_guard_active` 和 `dump_block_required`，将状态设置为 `RESTORE_DEFERRED`，返回 `CAMERA_RESTORE_NOT_IMPLEMENTED`，并记录真实命令处理耗时。

本轮 RESTORE 不调用 `HAL_DCMI_Start_DMA`，也不调用 `HAL_DMA_Abort`、`HAL_DMA_DeInit`，不切换 GPIO，不初始化 SDIO。RESTORE 后 basic、pc_dump、repeat 能否恢复，由板测判断。

### 6. DUMP 保护策略保持

- `software_guard_active=1` 时，文本 DUMP 继续被阻止并增加 `dump_block_count`。
- `software_guard_active=1` 时，合法二进制图像请求继续被阻止并增加 `binary_block_count`；PC 端接收超时是预期现象。
- 不修改 OV56RGB5 图像帧或二进制请求帧格式，不修改 Python 工具。
- RESTORE 清除 guard 后，DUMP 与二进制请求重新获准进入原有处理链路。

### 7. 风险说明

- 如果当前项目的 DUMP 流程会为每次请求重新配置并启动 DCMI DMA，RESTORE 后图像导出可能正常恢复。
- 如果现有流程依赖停止前的持续采集状态，RESTORE 后可能无法导出；下一阶段需要单独设计 `HAL_DCMI_Start_DMA` 恢复路径。
- `HAL_DCMI_Stop` 返回失败时 guard 仍保持激活，避免新的 DUMP 请求进入状态不确定的采集链路。
- 本轮不处理 GPIO 复用，不会切换 PC8、PC9、PC11，也不处理 SDIO 或 FATFS。

### 8. 板测计划

1. 启动后执行 `SNAPSHOT STATUS`，确认 `real_dcmi_stop_enabled=1` 且 stop 计数均为 0。
2. guard 前执行 `basic`，确认 PASS。
3. 执行 `SNAPSHOT PREPARE`，检查 CLI 提示和 `last_dcmi_stop_hal_status`。
4. 确认 `dcmi_stop_attempt_count=1`。
5. 若返回 `HAL_OK`，确认成功计数为 1、错误计数为 0、状态为 `CAMERA_PAUSED`。
6. 若返回非 `HAL_OK`，确认成功计数为 0、错误计数为 1、错误文本为 `DCMI_STOP_FAILED`，同时系统无复位或 FATAL。
7. guard 状态下执行文本 DUMP，确认被阻止且无 OV56RGB5 图像帧。
8. guard 状态下执行合法二进制图像请求，确认被阻止或 PC 端超时。
9. 执行 `SNAPSHOT RESTORE`，确认 `software_guard_active=0`、`dump_block_required=0`。
10. RESTORE 后依次执行 basic、pc_dump、repeat 20/20，判断原有 DUMP 流程是否能自行恢复采集。
11. 执行最终 `STATUS`，检查 IWDG、HOOK 和 UART RX DMA，无复位、Hook 故障、UART 错误或 StreamBuffer 溢出。

本轮 Codex 不执行硬件测试；上述结果由用户烧录后在开发板上验证并回填。

### 9. Stage 11B-6 板测结果

#### 9.1 启动与初始状态

系统启动正常，启动信息为 `reset: iwdg=0`；测试期间无 FATAL、无反复复位、无 IWDG 复位循环。

初始 `SNAPSHOT STATUS`：

```text
camera_control_state=0
camera_control_state_text=IDLE
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=0
dcmi_stop_success_count=0
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=0
binary_block_count=0
```

激活 guard 前执行 `basic`，结果 PASS，`frame_id=1`，CRC 校验一致。

#### 9.2 SNAPSHOT PREPARE 与真实 DCMI Stop 结果

执行 `SNAPSHOT PREPARE` 后输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

`HAL_DCMI_Stop` 返回 `HAL_OK`，状态记录如下：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
last_error_code=0
last_error_text=OK
software_guard_active=1
dump_block_required=1
```

结果表明真实 DCMI Stop 执行成功，系统进入 `CAMERA_PAUSED`，软件保护同时保持激活。

#### 9.3 guard 状态下的请求阻止结果

guard 状态下执行文本 `DUMP`：

- 输出 `DUMP blocked: snapshot software guard active.`。
- 未发送 OV56RGB5 图像帧。
- `dump_block_count` 从 0 增加到 1。

guard 状态下执行二进制 `basic`：

- 响应长度为 0 B，PC 端接收超时，只收到 0/38426 B，测试结果为 FAIL。
- 该 FAIL 是本轮软件保护生效后的预期现象。
- `binary_block_count` 从 0 增加到 1。
- 未增加协议解析错误。

#### 9.4 SNAPSHOT RESTORE 与图像功能回归

执行 `SNAPSHOT RESTORE` 后输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

RESTORE 后状态：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
restore_success_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
```

软件保护解除后的图像功能回归结果：

- `basic`：PASS，`frame_id=2`。
- `pc_dump`：PASS，`frame_id=3`，图像质量无警告。
- `repeat`：20/20 PASS，`frame_id` 从 4 到 23 连续。
- `repeat` 平均耗时 3465.81 ms，最短耗时 3449.45 ms，最长耗时 3468.23 ms。

#### 9.5 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=25
dump_success_count=23
dump_error_count=2
binary_request_count=23
binary_request_success_count=22
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
last_dump_time_ms=3513
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和二进制图像请求各被阻止一次。`binary_request_error_count=0` 是正确结果：二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于本轮预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7648
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=79
monitor_heartbeat_age_ms=79
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=924
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=40
uart_dma_rx_bytes=462
stream_buffer_write_bytes=462
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 10. Stage 11B-6 板测结论

Stage 11B-6 验证通过。`SNAPSHOT PREPARE` 中真实调用 `HAL_DCMI_Stop` 后返回 `HAL_OK`，系统进入 `CAMERA_PAUSED` 状态，软件保护状态生效；guard 状态下文本 DUMP 和二进制图像请求均被阻止，未发送 OV56RGB5 图像帧。`SNAPSHOT RESTORE` 清除 guard 后，basic、pc_dump、repeat 均恢复正常。

最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明本轮真实 `HAL_DCMI_Stop` 最小验证没有破坏现有系统稳定性。

需要严谨说明：RESTORE 后 basic、pc_dump、repeat 能恢复，说明当前工程在 `HAL_DCMI_Stop` 后仍可恢复图像导出；但本轮并未实现显式 `HAL_DCMI_Start_DMA` 恢复路径，因此目前不能仅凭本轮结果断定具体恢复机制。后续仍需确认恢复是由现有 DUMP 路径重新启动采集，还是由现有帧缓存或其他采集机制支撑。

### 11. 后续 Stage 11B-7 建议

- 执行多轮 `SNAPSHOT PREPARE` / `SNAPSHOT RESTORE` 循环测试。
- 暂时仍不切换 PC8、PC9、PC11，也不初始化 SDIO。
- 验证多次调用 `HAL_DCMI_Stop` 后系统是否仍稳定，无死机、复位或 IWDG 异常。
- 验证每轮 RESTORE 后 `basic` 是否都能恢复，并检查 frame_id、CRC 和耗时。
- 根据多轮测试结果确认现有恢复机制，再决定是否需要显式 `HAL_DCMI_Start_DMA` 恢复路径。

## Stage 11B-7 多轮 Stop/Restore 循环稳定性验证

### 1. 本轮目的

- 新增独立测试工具 `tools/uart_snapshot_cycle_test.py`。
- 自动执行多轮 `SNAPSHOT PREPARE` / `SNAPSHOT RESTORE` 循环。
- 验证多次执行现有 `HAL_DCMI_Stop` 后系统是否保持稳定。
- 验证每轮 guard 状态下文本 DUMP 和二进制图像请求持续被阻止。
- 验证每轮 RESTORE 清除 guard 后，二进制图像请求持续恢复并通过帧格式及 CRC 校验。
- 检查 RESTORE 后有效图像帧的 `frame_id` 是否连续递增。

### 2. 本轮明确不做

- 不修改任何固件 C/H 源码，包括 `camera_snapshot_control`、`camera_rtos` 和 `camera_cli`。
- 不新增或修改 `HAL_DCMI_Stop` 调用，不调用 `HAL_DCMI_Start_DMA`。
- 不调用 `HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不修改 DCMI、DMA、UART DMA、二进制请求协议或 OV56RGB5 图像帧格式。
- 不释放或切换 PC8、PC9、PC11，不出现 `GPIO_AF12_SDIO`。
- 不初始化 SDIO，不接入 FATFS，不读写 SD 卡。
- 不修改或导入现有 Python 工具，不保存 PNG，不做图像质量分析。

### 3. 循环测试脚本

脚本路径为 `tools/uart_snapshot_cycle_test.py`，只依赖 pyserial 和 Python 标准库。默认参数如下：

```text
--port COM4
--baud 115200
--cycles 5
--guard-timeout 2.0
--frame-timeout 10.0
--interval 0.2
--tag stage11_b7_snapshot_cycle
```

脚本在打开串口前设置 `rtscts=False`、`dsrdtr=False`、`DTR=False` 和 `RTS=False`，打开后打印串口、波特率、循环次数以及 DTR/RTS 状态。

每轮测试顺序：

1. 发送 `SNAPSHOT PREPARE`，确认响应包含 `DCMI stop OK`。
2. 发送文本 `DUMP`，确认响应为 `DUMP blocked: snapshot software guard active.`，且未进入 OV56RGB5 发送流程。
3. 发送一帧合法 14 字节二进制图像请求；guard 状态下没有收到合法 OV56RGB5 图像帧即为 PASS，0 B 或超时均属于预期。
4. 发送 `SNAPSHOT RESTORE`，确认响应包含 `SNAPSHOT RESTORE` 和 `RESTORE_DEFERRED`。
5. 再次发送二进制图像请求；此时必须收到完整 38426 B OV56RGB5 帧，并通过版本、像素格式、160×120 尺寸、38400 B payload 和 payload CRC32 校验。
6. 记录恢复帧的 `frame_id` 和耗时，然后等待 `interval` 秒进入下一轮。

单项失败时脚本打印失败原因、写入该轮 CSV 记录，并默认继续后续步骤和下一轮；只有串口打开失败时直接结束。

### 4. 测试统计与输出

脚本统计以下结果：

- `cycle_total`
- `prepare_ok_count`
- `text_dump_block_ok_count`
- `binary_block_ok_count`
- `restore_command_ok_count`
- `restore_binary_ok_count`
- `fail_count`
- `first_frame_id`
- `last_frame_id`
- `frame_id_continuous`
- `avg_restore_binary_time_ms`
- `min_restore_binary_time_ms`
- `max_restore_binary_time_ms`

脚本自动创建 `captures` 目录并输出：

```text
captures/snapshot_cycle_<tag>_<timestamp>.csv
captures/snapshot_cycle_<tag>_<timestamp>_summary.txt
```

CSV 保存每轮五项结果、frame_id、RESTORE 后二进制请求耗时和错误原因。summary 保存测试参数、全部统计、frame_id 连续性和最终 PASS/FAIL，并明确说明 guard 状态下二进制请求超时属于预期、RESTORE 后二进制请求必须 PASS。

总测试仅在所有循环的 PREPARE、文本 DUMP 阻止、二进制请求阻止、RESTORE 命令和 RESTORE 后二进制请求均通过，且恢复帧 `frame_id` 连续递增、串口过程无异常中断时判定为 PASS。

### 5. 后续板测计划

1. 先执行 5 轮测试：

   ```text
   python tools/uart_snapshot_cycle_test.py --cycles 5 --tag stage11_b7_5cycle
   ```

2. 若 5 轮通过，再执行 20 轮测试：

   ```text
   python tools/uart_snapshot_cycle_test.py --cycles 20 --tag stage11_b7_20cycle
   ```

3. 测试完成后通过串口执行 `STATUS`，检查 IWDG、Hook、心跳、堆栈、UART RX DMA、StreamBuffer 和图像请求统计。
4. 若 20 轮全部通过且运行保护状态正常，再进入下一阶段。

本轮 Codex 只进行脚本静态编译检查，不打开串口、不执行硬件测试；循环结果由用户烧录现有固件后在开发板上验证并回填。

### 6. 后续 Stage 11B-8 建议

- 如果 Stage 11B-7 多轮测试稳定，结合测试结果继续梳理是否需要显式 `HAL_DCMI_Start_DMA` 恢复路径。
- 如果现有 DUMP 路径能够持续自行恢复，则进入“释放/恢复 PC8、PC9、PC11 的软件状态设计”。
- 如果多轮测试暴露恢复失败或 frame_id 异常，则先进行显式 DCMI Start 恢复验证，再处理冲突引脚。

### 7. Stage 11B-7 板测结果

#### 7.1 5 轮循环测试

执行命令：

```text
python tools/uart_snapshot_cycle_test.py --cycles 5 --tag stage11_b7_5cycle
```

测试统计：

```text
cycle_total=5
prepare_ok_count=5
text_dump_block_ok_count=5
binary_block_ok_count=5
restore_command_ok_count=5
restore_binary_ok_count=5
fail_count=0
first_frame_id=1
last_frame_id=5
frame_id_continuous=是
avg_restore_binary_time_ms=3442.12
min_restore_binary_time_ms=3433.17
max_restore_binary_time_ms=3451.42
测试结果=PASS
```

输出文件：

```text
captures\snapshot_cycle_stage11_b7_5cycle_20260805_131553.csv
captures\snapshot_cycle_stage11_b7_5cycle_20260805_131553_summary.txt
```

#### 7.2 20 轮循环测试

执行命令：

```text
python tools/uart_snapshot_cycle_test.py --cycles 20 --tag stage11_b7_20cycle
```

测试统计：

```text
cycle_total=20
prepare_ok_count=20
text_dump_block_ok_count=20
binary_block_ok_count=20
restore_command_ok_count=20
restore_binary_ok_count=20
fail_count=0
first_frame_id=6
last_frame_id=25
frame_id_continuous=是
avg_restore_binary_time_ms=3445.54
min_restore_binary_time_ms=3426.49
max_restore_binary_time_ms=3486.38
测试结果=PASS
```

输出文件：

```text
captures\snapshot_cycle_stage11_b7_20cycle_20260805_131629.csv
captures\snapshot_cycle_stage11_b7_20cycle_20260805_131629_summary.txt
```

#### 7.3 合计 25 轮结果

5 轮和 20 轮连续测试均通过，合计完成 25 轮 `SNAPSHOT PREPARE` / `DUMP_BLOCK` / `BINARY_BLOCK` / `SNAPSHOT RESTORE` / `RESTORE_BINARY` 循环：

- 25 次 `SNAPSHOT PREPARE` 均成功进入 `CAMERA_PAUSED` 和 guard 状态。
- guard 状态下 25 次文本 DUMP 均被阻止。
- guard 状态下 25 次二进制图像请求均未收到合法 OV56RGB5 图像帧，属于预期现象。
- `SNAPSHOT RESTORE` 后 25 次二进制图像请求均恢复 PASS。
- RESTORE 后 `frame_id` 从 1 到 25 连续递增。
- 测试期间未观察到复位、FATAL 或串口异常。

### 8. 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=75
dump_success_count=25
dump_error_count=50
binary_request_count=50
binary_request_success_count=25
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=40
last_binary_error_code=0
last_error_code=8
```

统计解释：

- `dump_request_count=75` 是预期结果，对应每轮 1 次 guard 文本 DUMP、1 次 guard binary 请求和 1 次 RESTORE 后 binary 请求。
- `dump_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 图像导出成功。
- `dump_error_count=50` 是预期结果，对应 25 次 guard 文本 DUMP 阻止和 25 次 guard binary 阻止。
- `binary_request_count=50` 是预期结果，对应 25 次 guard binary 请求和 25 次 RESTORE 后 binary 请求。
- `binary_request_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 请求成功。
- `binary_request_error_count=0` 是正确结果，因为 guard 状态下二进制请求格式正确，只是被 snapshot guard 拦截，不属于协议错误。
- `last_error_code=8` 对应 snapshot guard active 类错误，属于 guard 阻止路径的预期记录。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7648
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=5
monitor_heartbeat_age_ms=504
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=1402
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=152
uart_dma_rx_bytes=1683
stream_buffer_write_bytes=1683
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 9. Stage 11B-7 板测结论与适用边界

Stage 11B-7 验证通过。新增 `uart_snapshot_cycle_test.py` 后，5 轮和 20 轮多轮 Stop/Restore 循环测试均 PASS。合计 25 轮测试中，`SNAPSHOT PREPARE`、guard 文本 DUMP 阻止、guard binary 阻止、`SNAPSHOT RESTORE`、RESTORE 后 binary 图像恢复均正常；RESTORE 后 `frame_id` 从 1 到 25 连续递增。

最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明多轮 `HAL_DCMI_Stop` + guard + RESTORE 软件流程稳定。

需要严谨说明：当前多轮测试说明 `HAL_DCMI_Stop` 在本工程中可反复安全执行，且 RESTORE 清除 guard 后图像请求可以恢复。但是本阶段仍未切换 PC8、PC9、PC11，也未初始化 SDIO 或 FATFS，因此该结果不能证明 SDIO 接管后的相机恢复一定可靠。

### 10. 后续 Stage 11B-8 建议

Stage 11B-8 建议进入“冲突引脚释放/恢复的软件状态设计与安全检查”，仍先不真正切换 PC8、PC9、PC11：

- 明确 DCMI 停止并进入 `CAMERA_PAUSED` 后，允许 `SD TAKEOVER ENTER` 的前置条件。
- 明确接管和恢复过程完成前禁止 DUMP 与二进制图像请求的条件。
- 明确任何接管异常路径都必须进入可执行 RESTORE 的安全状态。
- 先设计状态关联、顺序检查、错误码和回滚边界，再进入真实 GPIO 复用切换。

## Stage 11B-8 SDIO接管前置条件检查

### 1. 本轮目的

- 为 `SD TAKEOVER ENTER` 增加 SNAPSHOT 状态前置检查。
- 没有先执行 `SNAPSHOT PREPARE` 并成功进入 `CAMERA_PAUSED` 时，禁止进入 SDIO 接管流程。
- `SNAPSHOT PREPARE` 成功后，允许 `SD TAKEOVER ENTER` 进入 `ENTER_DEFERRED`，但仍不执行 GPIO 切换。
- `SNAPSHOT RESTORE` 清除软件 guard 后，前置条件自动失效，再次执行 `SD TAKEOVER ENTER` 必须被阻止。
- 记录前置检查的尝试、成功、失败和最近错误状态，为后续真实冲突引脚释放提供安全门。

### 2. 本轮明确不做

- 不新增 `HAL_DCMI_Stop`，保留 Stage 11B-6 已有调用。
- 不调用 `HAL_DCMI_Start_DMA`、`HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不释放或切换 PC8、PC9、PC11，不出现 `GPIO_AF12_SDIO`。
- 不初始化 SDIO，不调用 SD 卡块读写接口。
- 不接入 FATFS，不读写 SD 卡或文件。
- 不修改 DUMP、二进制请求协议、OV56RGB5 图像帧、UART DMA、IWDG 或 FreeRTOS 任务配置。
- 不修改现有 Python 工具。

### 3. SNAPSHOT 只读查询接口

新增两个只读软件状态接口：

- `Camera_SnapshotControl_IsCameraPausedForSnapshot()`：仅在 `camera_control_state == CAMERA_SNAPSHOT_STATE_CAMERA_PAUSED` 时返回 1。
- `Camera_SnapshotControl_IsTakeoverPreconditionReady()`：仅在相机处于 `CAMERA_PAUSED`，且 `software_guard_active == 1`、`dump_block_required == 1` 三项同时满足时返回 1。

两个接口只读取 SNAPSHOT 软件状态，不修改状态，也不访问 DCMI、DMA、GPIO、SDIO 或任何 HAL API。`camera_sd_storage` 仅在处理 ENTER 请求时调用查询接口，`SNAPSHOT RESTORE` 不反向修改 SD 模块状态，从而避免两个模块双向强耦合。

### 4. 新增 SD 接管前置检查字段

| 字段 | 含义 |
| --- | --- |
| `takeover_precheck_required` | ENTER 前是否必须执行 SNAPSHOT 状态检查，本项目固定为 1。 |
| `takeover_precheck_attempt_count` | `SD TAKEOVER ENTER` 前置检查次数。 |
| `takeover_precheck_success_count` | 前置检查成功次数。 |
| `takeover_precheck_fail_count` | 前置检查失败次数。 |
| `snapshot_pause_required` | 是否要求相机处于暂停状态，本项目固定为 1。 |
| `snapshot_pause_confirmed` | 最近一次前置检查是否确认相机已暂停。 |
| `conflict_pin_release_ready` | 软件条件是否允许进入冲突引脚释放流程；不表示 GPIO 已释放。 |
| `last_takeover_precheck_error_code` | 最近一次前置检查错误码。 |

这些字段以及 `last_takeover_precheck_error_text` 同时追加到 `SD STATUS` 和 `SD TAKEOVER STATUS`，原有字段名保持不变。新增错误码 `SNAPSHOT_NOT_PAUSED` 和 `TAKEOVER_PRECHECK_FAILED`。

### 5. SD TAKEOVER ENTER 安全门行为

每次执行 `SD TAKEOVER ENTER` 都增加 ENTER 尝试次数和 precheck 尝试次数，并实时调用 `Camera_SnapshotControl_IsTakeoverPreconditionReady()`。

前置条件不满足时：

- 增加 `takeover_precheck_fail_count`。
- 将 `snapshot_pause_confirmed` 和 `conflict_pin_release_ready` 清零。
- `takeover_state` 保持或恢复为 `IDLE`，避免误认为接管流程已经开始。
- 最近接管错误码和前置检查错误码均记录为 `SNAPSHOT_NOT_PAUSED`。
- CLI 输出 `SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.`。
- 不修改 `sdio_ready` 或 `is_initialized`，不访问 GPIO、SDIO、FATFS。

前置条件满足时：

- 增加 `takeover_precheck_success_count`。
- 将 `snapshot_pause_confirmed` 和 `conflict_pin_release_ready` 置 1。
- 将 `takeover_state` 设置为 `ENTER_DEFERRED`，不设置为 `ACTIVE`。
- 前置检查错误码清为 `OK`，接管返回码仍为 `TAKEOVER_NOT_IMPLEMENTED`。
- CLI 输出 `SD TAKEOVER ENTER: precheck OK, GPIO switch is not implemented yet.`。
- 不释放或切换 PC8、PC9、PC11，不初始化 SDIO 或 FATFS。

### 6. EXIT、RESTORE 与 SD INIT 行为

`SD TAKEOVER EXIT` 仍只进入 `EXIT_DEFERRED`，同时清除 `snapshot_pause_confirmed` 和 `conflict_pin_release_ready`，CLI 输出 `SD TAKEOVER EXIT: deferred, GPIO restore is not implemented yet.`，不执行 GPIO 恢复。

`SNAPSHOT RESTORE` 继续只清除 SNAPSHOT guard，不主动修改 `camera_sd_storage`。RESTORE 后 `Camera_SnapshotControl_IsTakeoverPreconditionReady()` 返回 0，因此下一次 `SD TAKEOVER ENTER` 会重新检查并返回 `SNAPSHOT_NOT_PAUSED`。

`SD INIT` 行为保持不变，继续返回 `NEED_TAKEOVER`；不调用 `HAL_SD_Init`，不初始化 SDIO，不接入 FATFS，也不切换 GPIO。

### 7. 预期命令行为

1. 启动后直接执行 `SD TAKEOVER ENTER`：被阻止，提示先运行 `SNAPSHOT PREPARE`，状态保持 `IDLE`。
2. 执行 `SNAPSHOT PREPARE` 后再执行 `SD TAKEOVER ENTER`：前置检查通过，状态进入 `ENTER_DEFERRED`，但提示 GPIO switch 尚未实现。
3. guard 状态下文本 DUMP 和二进制图像请求仍被阻止。
4. 执行 `SD TAKEOVER EXIT`：进入 `EXIT_DEFERRED` 并清除接管准备标志，不恢复 GPIO。
5. 执行 `SNAPSHOT RESTORE` 后再次执行 `SD TAKEOVER ENTER`：前置检查再次失败并返回 `SNAPSHOT_NOT_PAUSED`。
6. 执行 `SD INIT`：仍返回 `NEED_TAKEOVER`。

### 8. 后续板测计划

1. 启动后检查 `SD STATUS` 和 `SD TAKEOVER STATUS` 的新增字段初始值。
2. 直接执行 `SD TAKEOVER ENTER`，确认被阻止、失败计数增加且状态为 `IDLE`。
3. 执行 `SNAPSHOT PREPARE`，确认进入 `CAMERA_PAUSED`、guard 生效。
4. 再执行 `SD TAKEOVER ENTER`，确认 precheck 成功计数增加、`snapshot_pause_confirmed=1`、`conflict_pin_release_ready=1`、状态为 `ENTER_DEFERRED`。
5. guard 状态下分别验证文本 DUMP 和合法二进制图像请求仍被阻止。
6. 执行 `SD TAKEOVER EXIT`，确认输出 deferred，且两个软件准备标志清零。
7. 执行 `SNAPSHOT RESTORE`，然后再次执行 `SD TAKEOVER ENTER`，确认再次被阻止。
8. RESTORE 后执行 basic、pc_dump、repeat 20/20 回归。
9. 执行最终 `STATUS`，检查 IWDG、HOOK、心跳、UART RX DMA 和 StreamBuffer 状态。

本轮 Codex 不执行硬件测试；上述命令和回归结果由用户烧录后在开发板上验证并回填。

### 9. 后续 Stage 11B-9 建议

- 在软件前置检查和顺序保护板测稳定后，设计 PC8、PC9、PC11 真实 GPIO 释放/恢复最小验证。
- Stage 11B-9 仍先不初始化 SDIO 或 FATFS，只验证冲突引脚能从 DCMI 复用态安全切出并恢复为 DCMI。
- 真实切出前必须确认相机处于 `CAMERA_PAUSED`、guard 已激活且 precheck 已通过。
- 恢复 DCMI 复用后验证 basic、pc_dump、repeat 和多轮 Stop/Restore 链路能否继续正常运行。

### 10. Stage 11B-8 板测结果

#### 10.1 启动与首次安全门检查

系统启动正常，启动信息为 `reset: iwdg=0`；测试期间无 FATAL、无反复复位、无 IWDG 复位循环。

启动后直接执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

状态如下：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=0
takeover_precheck_fail_count=1
snapshot_pause_required=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
last_takeover_precheck_error_code=9
last_takeover_precheck_error_text=SNAPSHOT_NOT_PAUSED
```

结果表明未暂停摄像头时安全门能够阻止接管请求，接管状态保持 `IDLE`。

#### 10.2 SNAPSHOT PREPARE 与前置检查通过

执行 `SNAPSHOT PREPARE`，输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

SNAPSHOT 状态：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

随后执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: precheck OK, GPIO switch is not implemented yet.
```

接管状态：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=2
takeover_precheck_attempt_count=2
takeover_precheck_success_count=1
takeover_precheck_fail_count=1
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
last_takeover_precheck_error_code=0
last_takeover_precheck_error_text=OK
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

结果表明 `CAMERA_PAUSED`、`software_guard_active=1` 和 `dump_block_required=1` 同时满足后，前置检查通过并进入 `ENTER_DEFERRED`；本轮仍未切换 GPIO。

#### 10.3 guard 状态下 DUMP 与 binary 阻止

- 文本 DUMP 输出 `DUMP blocked: snapshot software guard active.`。
- 未发送 OV56RGB5 图像帧，`dump_block_count=1`。
- guard 状态下二进制 basic 响应长度为 0 B，PC 端接收超时，测试结果为 FAIL。
- 该 FAIL 是 guard 生效后的预期现象，`binary_block_count=1`。

#### 10.4 SD TAKEOVER EXIT 与 SNAPSHOT RESTORE

执行 `SD TAKEOVER EXIT`，输出：

```text
SD TAKEOVER EXIT: deferred, GPIO restore is not implemented yet.
```

状态如下：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
```

执行 `SNAPSHOT RESTORE`，输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

SNAPSHOT 状态：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

RESTORE 后再次执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

接管状态：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=3
takeover_precheck_attempt_count=3
takeover_precheck_success_count=1
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
last_takeover_precheck_error_code=9
last_takeover_precheck_error_text=SNAPSHOT_NOT_PAUSED
```

结果表明 RESTORE 清除 guard 后前置条件正确失效，新的 ENTER 请求再次被阻止。

#### 10.5 SD INIT 保持原有行为

执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

状态如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
```

该结果说明 `SD INIT` 仍未真实初始化 SDIO、未接入 FATFS、未切换 PC8、PC9、PC11。`last_operation_ms=118564` 仍更像系统 tick，而不是本次命令耗时，属于既有耗时统计问题，后续单独修正，不影响本轮安全门验证结论。

#### 10.6 RESTORE 后图像功能回归

- 首次 guard 状态下执行 binary basic，响应长度为 0 B、结果 FAIL，属于预期阻止现象。
- 测试期间出现一次 COM4 `PermissionError`，原因为 PC 端串口被其他程序占用，不属于固件错误；释放串口占用后继续测试。
- RESTORE 后 `basic`：PASS，`frame_id=1`。
- RESTORE 后 `pc_dump`：PASS，`frame_id=2`，图像质量无警告。
- RESTORE 后 `repeat`：20/20 PASS，`frame_id` 从 3 到 22 连续。
- `repeat` 平均耗时 3466.81 ms，最短耗时 3461.54 ms，最长耗时 3471.45 ms。

### 11. 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 guard 状态下 binary 请求各被阻止一次。`binary_request_error_count=0` 是正确结果，因为二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于本轮预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7624
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=0
monitor_heartbeat_age_ms=576
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=186
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=48
uart_dma_rx_bytes=578
stream_buffer_write_bytes=578
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 12. Stage 11B-8 板测结论与适用边界

Stage 11B-8 验证通过。`SD TAKEOVER ENTER` 的前置条件检查生效：未执行 `SNAPSHOT PREPARE` 时会被阻止；`SNAPSHOT PREPARE` 成功进入 `CAMERA_PAUSED` 且 `software_guard_active=1` 后，`SD TAKEOVER ENTER` 前置检查通过并进入 `ENTER_DEFERRED`，但仍不切换 GPIO；`SNAPSHOT RESTORE` 后前置条件失效，`SD TAKEOVER ENTER` 再次被阻止。

`SD INIT` 仍保持 `NEED_TAKEOVER`。RESTORE 后 basic、pc_dump、repeat 回归通过，最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步。

需要严谨说明：本轮只验证了 SDIO 接管前的软件安全门，没有真正释放或切换 PC8、PC9、PC11，也没有初始化 SDIO 或 FATFS。因此本轮通过只能说明“接管前置条件判断正确”，不能说明真实 SDIO 接管已经可用。

### 13. 后续 Stage 11B-9 建议

Stage 11B-9 建议进入“冲突引脚释放/恢复的软件状态设计与最小验证”。仍先不初始化 SDIO 或 FATFS，先验证 PC8、PC9、PC11 从 DCMI 复用切出再恢复后，basic、pc_dump、repeat 是否还能正常。

## Stage 11B-9 冲突引脚释放/恢复最小验证

### 1. 本轮目的

- `SD TAKEOVER ENTER` 前置检查通过后，将 PC8、PC9、PC11 从 DCMI AF13 释放为安全 GPIO 输入态。
- `SD TAKEOVER EXIT` 时，只将 PC8、PC9、PC11 恢复为 DCMI AF13。
- 记录释放/恢复尝试、成功、错误、当前释放状态、最近错误码和真实操作耗时。
- 验证不使用 `GPIO_AF12_SDIO`、不初始化 SDIO/FATFS 的情况下，引脚释放/恢复不会破坏 RESTORE 后的图像导出。

### 2. 本轮明确不做

- 不配置 `GPIO_AF12_SDIO`，不把任何引脚切换成 SDIO 复用。
- 不配置 PC10、PC12、PD2，不处理 SDIO 时钟、CMD 或 D2 数据线。
- 不调用 `HAL_SD_Init` 或其他 SD 卡块读写接口。
- 不接入 FATFS，不读写 SD 卡或文件。
- 不调用 `HAL_DCMI_Start_DMA`、`HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不重新初始化 OV5640，不重新初始化 DCMI 外设。
- 不修改 DUMP、二进制协议、OV56RGB5 帧格式、UART DMA、IWDG、FreeRTOS 任务配置或 Python 工具。

### 3. 冲突引脚释放策略

`Camera_SDStorage_ReleaseConflictPins()` 是 `camera_sd_storage.c` 内部静态函数，不向其他模块暴露。该函数只配置：

```text
PC8
PC9
PC11
```

释放配置：

```text
GPIO_MODE_INPUT
GPIO_NOPULL
GPIO_SPEED_FREQ_LOW
```

函数入口记录 tick 并增加释放尝试次数，调用 `HAL_GPIO_Init(GPIOC, &gpio)` 后回读 GPIOC 的 `MODER` 和 `PUPDR`，确认三个引脚均为输入且无上下拉。成功时增加释放成功次数、设置 `conflict_pins_released=1` 并记录 `OK`；回读不符时增加释放错误次数、状态保持未释放并返回 `CONFLICT_PIN_RELEASE_FAILED`。操作耗时使用出口 tick 减入口 tick。

### 4. 冲突引脚恢复策略

`Camera_SDStorage_RestoreConflictPins()` 同样是内部静态函数，只配置 PC8、PC9、PC11：

```text
GPIO_MODE_AF_PP
GPIO_NOPULL
GPIO_SPEED_FREQ_VERY_HIGH
GPIO_AF13_DCMI
```

恢复后回读 `MODER`、`PUPDR`、`OSPEEDR`、`OTYPER` 和 `AFR[1]`，确认三个引脚为 AF 推挽、无上下拉、超高速且复用为 AF13。成功时增加恢复成功次数并清除 `conflict_pins_released`；回读不符时增加恢复错误次数、保守保持释放标志并返回 `CONFLICT_PIN_RESTORE_FAILED`。即使当前 `conflict_pins_released=0`，EXIT 仍允许执行一次恢复配置。

恢复函数不调用 `HAL_DCMI_Start_DMA`，不重新初始化 DCMI 或 OV5640；恢复后图像链路能否正常工作由板测判断。

### 5. 新增状态字段

| 字段 | 含义 |
| --- | --- |
| `conflict_pin_release_attempt_count` | ENTER 中尝试释放 PC8、PC9、PC11 的次数。 |
| `conflict_pin_release_success_count` | 三个冲突引脚成功配置为输入态的次数。 |
| `conflict_pin_release_error_count` | 释放配置或回读检查失败次数。 |
| `conflict_pin_restore_attempt_count` | EXIT 中尝试恢复三个冲突引脚的次数。 |
| `conflict_pin_restore_success_count` | 三个冲突引脚成功恢复为 DCMI AF13 的次数。 |
| `conflict_pin_restore_error_count` | 恢复配置或回读检查失败次数。 |
| `conflict_pins_released` | PC8、PC9、PC11 当前是否处于释放状态。 |
| `last_conflict_pin_error_code` | 最近一次释放/恢复错误码。 |
| `last_conflict_pin_operation_ms` | 最近一次释放/恢复操作的真实处理耗时。 |

这些字段以及 `last_conflict_pin_error_text` 同时追加到 `SD STATUS` 和 `SD TAKEOVER STATUS`，原有字段保持不变。新增错误文本 `CONFLICT_PIN_RELEASE_FAILED`、`CONFLICT_PIN_RESTORE_FAILED` 和 `CONFLICT_PIN_NOT_RELEASED`。

### 6. SD TAKEOVER ENTER 行为

Stage 11B-8 的 SNAPSHOT 前置检查保持不变：

- 前置条件失败时，ENTER 保持 `IDLE`，返回 `SNAPSHOT_NOT_PAUSED`，不调用引脚释放函数。
- 前置条件成功时，确认相机暂停和 guard 状态后调用冲突引脚释放函数。
- 释放成功后进入 `ENTER_DEFERRED`，CLI 输出 `SD TAKEOVER ENTER: conflict pins released, GPIO switch to SDIO is not implemented yet.`。
- 即使释放成功仍返回 `TAKEOVER_NOT_IMPLEMENTED`，因为本轮没有配置 SDIO AF12 或初始化 SDIO。
- 释放失败时进入 `ERROR`，返回 `CONFLICT_PIN_RELEASE_FAILED`，不复位、不继续任何 SDIO 初始化。

### 7. SD TAKEOVER EXIT 与 SD INIT 行为

`SD TAKEOVER EXIT` 每次都尝试将三个冲突引脚恢复为 DCMI AF13：

- 恢复成功后清除 `conflict_pins_released`、`snapshot_pause_confirmed` 和 `conflict_pin_release_ready`，进入 `EXIT_DEFERRED`。
- CLI 输出 `SD TAKEOVER EXIT: conflict pins restored to DCMI AF13, SDIO restore is not implemented yet.`。
- 恢复失败时进入 `ERROR`，返回 `CONFLICT_PIN_RESTORE_FAILED`，不触发复位。

`SD INIT` 行为保持不变，仍返回 `NEED_TAKEOVER`；不调用 `HAL_SD_Init`，不初始化 SDIO，不接入 FATFS，也不配置 `GPIO_AF12_SDIO`。

### 8. 风险说明

- 本轮只验证 MCU 侧 GPIO 模式和复用切换，不代表 SDIO 接管已经完成或 SD 卡已经可访问。
- OV5640 传感器本身可能仍在输出 DVP 数据；本轮没有操作 PWDN，也没有通过寄存器停止传感器输出。
- 三个 MCU 引脚释放为输入态后不会主动驱动总线，但后续真实切换为 SDIO 输出/双向信号前，仍需评估 OV5640 DVP 输出与 SDIO 总线之间的物理冲突风险。
- 本轮不配置 AF12，因此不存在 MCU 通过 SDIO 外设访问 SD 卡的行为。
- DCMI AF13 恢复后不显式调用 `HAL_DCMI_Start_DMA`；图像恢复依赖现有请求链路，必须通过 basic、pc_dump、repeat 板测确认。

### 9. 后续板测计划

1. 启动后直接执行 `SD TAKEOVER ENTER`，确认未 PREPARE 时仍被阻止且释放尝试计数不增加。
2. 执行 `SNAPSHOT PREPARE`，确认进入 `CAMERA_PAUSED` 且 guard 生效。
3. 执行 `SD TAKEOVER ENTER`，确认释放尝试和成功计数增加、错误计数为 0、`conflict_pins_released=1`。
4. guard 状态下验证文本 DUMP 和合法二进制请求仍被阻止。
5. 执行 `SD TAKEOVER EXIT`，确认恢复尝试和成功计数增加、错误计数为 0、`conflict_pins_released=0`。
6. 执行 `SNAPSHOT RESTORE`，确认 guard 清零。
7. RESTORE 后执行 basic、pc_dump、repeat 20/20，确认图像链路恢复。
8. 执行 `SD INIT`，确认仍返回 `NEED_TAKEOVER`。
9. 执行最终 `STATUS`，检查 IWDG、HOOK、心跳、UART RX DMA 和 StreamBuffer 状态。

本轮 Codex 不执行硬件测试；上述引脚操作和图像恢复结果由用户烧录后在开发板上验证并回填。

### 10. 后续 Stage 11B-10 建议

- 如果本轮单次释放/恢复验证通过，下一步执行多轮 `SNAPSHOT PREPARE` / `SD TAKEOVER ENTER` / `SD TAKEOVER EXIT` / `SNAPSHOT RESTORE` 循环测试。
- Stage 11B-10 仍不初始化 SDIO 或 FATFS，重点验证多次 GPIO 输入态/AF13 切换后图像链路是否稳定。
- 每轮检查释放/恢复计数、错误码、`conflict_pins_released`、RESTORE 后 frame_id 和 CRC。

### 11. Stage 11B-9 板测结果

#### 11.1 启动与未 PREPARE 安全门检查

系统启动正常，启动信息为 `reset: iwdg=0`；OV5640 ID 为 `0x5640`，Camera init OK。测试期间无 FATAL、无反复复位、无 IWDG 复位循环。

启动后直接执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

状态如下：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=0
takeover_precheck_fail_count=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pins_released=0
conflict_pin_release_attempt_count=0
conflict_pin_release_success_count=0
conflict_pin_release_error_count=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
last_takeover_precheck_error_code=9
last_takeover_precheck_error_text=SNAPSHOT_NOT_PAUSED
```

未执行 `SNAPSHOT PREPARE` 时，ENTER 被正确阻止，PC8、PC9、PC11 未释放，释放尝试计数保持为 0。

#### 11.2 SNAPSHOT PREPARE 与冲突引脚释放

执行 `SNAPSHOT PREPARE`，输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

SNAPSHOT 状态：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

随后执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: conflict pins released, GPIO switch to SDIO is not implemented yet.
```

接管和引脚状态：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=2
takeover_precheck_attempt_count=2
takeover_precheck_success_count=1
takeover_precheck_fail_count=1
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pins_released=1
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
last_conflict_pin_operation_ms=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

PC8、PC9、PC11 已成功释放为 GPIO 输入态。本轮仍未切换为 `GPIO_AF12_SDIO`，未初始化 SDIO 或 FATFS；返回 `TAKEOVER_NOT_IMPLEMENTED` 是预期行为，因为真实 SDIO 接管尚未实现。`last_conflict_pin_operation_ms=0` 表示该 GPIO 操作在当前 tick 分辨率内完成。

#### 11.3 guard 状态下 DUMP 与 binary 阻止

- 文本 DUMP 输出 `DUMP blocked: snapshot software guard active.`。
- 未发送 OV56RGB5 图像帧，`dump_block_count=1`。
- guard 状态下二进制 basic 响应长度为 0 B，PC 端接收超时，测试结果为 FAIL。
- 该 FAIL 是 guard 生效后的预期现象，`binary_block_count=1`。

#### 11.4 冲突引脚恢复与 SNAPSHOT RESTORE

执行 `SD TAKEOVER EXIT`，输出：

```text
SD TAKEOVER EXIT: conflict pins restored to DCMI AF13, SDIO restore is not implemented yet.
```

状态如下：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
last_conflict_pin_operation_ms=0
```

PC8、PC9、PC11 已成功恢复为 DCMI AF13。本轮未调用 `HAL_DCMI_Start_DMA`，也未重新初始化 OV5640 或 DCMI 外设。

执行 `SNAPSHOT RESTORE`，输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

SNAPSHOT 状态：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

#### 11.5 SD INIT 保持原有行为

执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

状态如下：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
conflict_pins_released=0
last_conflict_pin_error_text=OK
```

`SD INIT` 仍保持 `NEED_TAKEOVER`，没有真实初始化 SDIO、接入 FATFS 或读写 SD 卡。`last_operation_ms=113359` 仍更像系统 tick，属于既有耗时统计问题，后续单独修正，不影响本轮引脚释放/恢复结论。

#### 11.6 RESTORE 后图像功能回归

- 首次 guard 状态下执行 binary basic，响应长度为 0 B、结果 FAIL，属于预期阻止现象。
- RESTORE 后 `basic`：PASS，`frame_id=1`。
- RESTORE 后 `pc_dump`：PASS，`frame_id=2`，图像质量无警告。
- RESTORE 后 `repeat`：20/20 PASS，`frame_id` 从 3 到 22 连续。
- `repeat` 平均耗时 3466.20 ms，最短耗时 3458.52 ms，最长耗时 3469.64 ms。

### 12. 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 guard 状态下 binary 请求各被阻止一次。`binary_request_error_count=0` 是正确结果，因为二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于本轮预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7592
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=57
monitor_heartbeat_age_ms=956
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=162
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=47
uart_dma_rx_bytes=568
stream_buffer_write_bytes=568
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 13. Stage 11B-9 板测结论与适用边界

Stage 11B-9 验证通过。未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 被正确阻止且未释放冲突引脚；`SNAPSHOT PREPARE` 成功后，ENTER 能将 PC8、PC9、PC11 释放为 GPIO 输入态，并记录 `conflict_pins_released=1`；`SD TAKEOVER EXIT` 能将三个引脚恢复为 DCMI AF13，并记录 `conflict_pins_released=0`。

随后 `SNAPSHOT RESTORE` 清除 guard，RESTORE 后 basic、pc_dump、repeat 均恢复正常。最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步。

需要严谨说明：本轮只验证了 MCU 侧 PC8、PC9、PC11 从 DCMI AF13 释放为 GPIO 输入态、再恢复为 DCMI AF13 的最小闭环。虽然 RESTORE 后图像导出恢复正常，但本轮没有切换为 `GPIO_AF12_SDIO`，没有初始化 SDIO 或 FATFS，也没有访问 SD 卡。因此本轮通过不能说明真实 SD 卡接管已经完成。

### 14. 后续 Stage 11B-10 建议

Stage 11B-10 建议新增 PC 端自动化脚本，执行多轮 `SNAPSHOT PREPARE` / `SD TAKEOVER ENTER` / `DUMP_BLOCK` / `BINARY_BLOCK` / `SD TAKEOVER EXIT` / `SNAPSHOT RESTORE` / `RESTORE_BINARY` 循环测试。仍不初始化 SDIO 或 FATFS，先验证多次 PC8、PC9、PC11 释放/恢复后图像链路是否稳定。

## Stage 11B-10 多轮冲突引脚释放/恢复循环稳定性验证

### 1. 本轮目的

- 新增独立测试工具 `tools/uart_snapshot_takeover_cycle_test.py`。
- 自动执行多轮 `SNAPSHOT PREPARE` / `SD TAKEOVER ENTER` / `SD TAKEOVER EXIT` / `SNAPSHOT RESTORE` 完整闭环。
- 验证 PC8、PC9、PC11 多次从 DCMI AF13 释放为输入态、再恢复为 DCMI AF13 后，图像链路是否稳定。
- 验证每轮 guard 状态下文本 DUMP 和二进制图像请求持续被阻止。
- 验证每轮 RESTORE 后二进制图像请求持续恢复，并通过帧格式与 CRC 校验。
- 检查 RESTORE 后有效图像帧的 `frame_id` 是否连续递增。

### 2. 本轮明确不做

- 不修改任何固件 C/H 源码，包括 `camera_sd_storage`、`camera_snapshot_control`、`camera_rtos` 和 `camera_cli`。
- 不新增或修改 `HAL_DCMI_Stop`，不调用 `HAL_DCMI_Start_DMA`。
- 不调用 `HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不修改 PC8、PC9、PC11 的固件切换实现，不配置 `GPIO_AF12_SDIO`。
- 不初始化 SDIO，不调用 SD 卡块读写接口。
- 不接入 FATFS，不读写 SD 卡或文件。
- 不修改二进制请求协议、OV56RGB5 图像帧、UART DMA、IWDG 或现有 Python 工具。
- 不保存 PNG，不做图像质量分析。

### 3. 循环测试脚本

脚本路径为 `tools/uart_snapshot_takeover_cycle_test.py`，只依赖 pyserial 和 Python 标准库，不导入现有 Python 工具。默认参数如下：

```text
--port COM4
--baud 115200
--cycles 5
--guard-timeout 2.0
--frame-timeout 10.0
--interval 0.2
--tag stage11_b10_takeover_cycle
```

脚本在打开串口前设置 `rtscts=False`、`dsrdtr=False`、`DTR=False` 和 `RTS=False`，打开后打印串口、波特率、循环次数以及 DTR/RTS 状态。文本命令以 `\r\n` 结尾，发送前清理输入缓冲区，响应使用 `errors="ignore"` 解码并只判断必要关键字。

每轮测试顺序：

1. 发送 `SNAPSHOT PREPARE`，必须确认响应包含 `DCMI stop OK`。
2. 发送 `SD TAKEOVER ENTER`，必须确认响应包含 `conflict pins released`；若出现 `blocked, run SNAPSHOT PREPARE first` 则本轮失败。
3. 发送文本 `DUMP`，必须确认响应为 `DUMP blocked: snapshot software guard active.`。
4. 发送一帧合法 14 字节二进制图像请求；guard 状态下没有收到合法 OV56RGB5 帧即为 PASS，0 B 或超时均属于预期。
5. 发送 `SD TAKEOVER EXIT`，必须确认响应包含 `conflict pins restored`，表示三个引脚已恢复为 DCMI AF13。
6. 发送 `SNAPSHOT RESTORE`，响应包含 `SNAPSHOT RESTORE` 或 `RESTORE_DEFERRED` 即认为命令响应正常。
7. 再次发送二进制图像请求；必须收到完整 38426 B OV56RGB5 帧，并通过版本、像素格式、160×120 尺寸、38400 B payload 和 payload CRC32 校验。
8. 记录恢复帧的 `frame_id` 和耗时，等待 `interval` 秒进入下一轮。

单项失败时脚本打印失败原因、写入该轮 CSV，并默认继续后续步骤和下一轮；只有串口打开失败时直接结束。

### 4. 测试统计与输出

脚本统计：

- `cycle_total`
- `prepare_ok_count`
- `takeover_enter_ok_count`
- `text_dump_block_ok_count`
- `binary_block_ok_count`
- `takeover_exit_ok_count`
- `restore_command_ok_count`
- `restore_binary_ok_count`
- `fail_count`
- `first_frame_id`
- `last_frame_id`
- `frame_id_continuous`
- `avg_restore_binary_time_ms`
- `min_restore_binary_time_ms`
- `max_restore_binary_time_ms`

脚本自动创建 `captures` 目录并输出：

```text
captures/snapshot_takeover_cycle_<tag>_<timestamp>.csv
captures/snapshot_takeover_cycle_<tag>_<timestamp>_summary.txt
```

CSV 保存每轮七项结果、frame_id、RESTORE 后二进制请求耗时和错误原因。summary 保存测试参数、全部统计、frame_id 连续性和最终 PASS/FAIL，并明确说明 guard binary 超时属于预期、ENTER 后引脚必须释放、EXIT 后引脚必须恢复为 DCMI AF13、RESTORE 后 binary 必须 PASS。

总测试仅在所有循环的 PREPARE、TAKEOVER ENTER、文本 DUMP 阻止、二进制请求阻止、TAKEOVER EXIT、RESTORE 命令和 RESTORE 后二进制请求均通过，且恢复帧 `frame_id` 连续递增、串口过程无异常中断时判定为 PASS。

### 5. 后续板测计划

1. 先执行 5 轮测试：

   ```text
   python tools/uart_snapshot_takeover_cycle_test.py --cycles 5 --tag stage11_b10_5cycle
   ```

2. 若 5 轮通过，再执行 20 轮测试：

   ```text
   python tools/uart_snapshot_takeover_cycle_test.py --cycles 20 --tag stage11_b10_20cycle
   ```

3. 测试完成后通过串口依次执行 `SD TAKEOVER STATUS`、`SNAPSHOT STATUS` 和 `STATUS`。
4. 确认 `conflict_pins_released=0`，释放/恢复成功计数符合循环次数且错误计数为 0。
5. 检查 IWDG、Hook、心跳、堆栈、UART RX DMA、StreamBuffer 和图像请求统计。
6. 若 20 轮全部通过且运行保护状态正常，再进入下一阶段。

本轮 Codex 只进行脚本静态编译检查，不打开串口、不执行硬件测试；循环结果由用户在开发板上验证并回填。

### 6. 后续 Stage 11B-11 建议

- 如果 Stage 11B-10 多轮释放/恢复稳定，开始设计“真实 SDIO AF12 切换但不初始化 SD 卡”的最小验证。
- 目标闭环为 PC8、PC9、PC11 从 DCMI AF13 切到 GPIO 输入态，再切到 SDIO AF12，然后退回 GPIO 输入态并恢复 DCMI AF13。
- Stage 11B-11 仍先不调用 `HAL_SD_Init`，不接入 FATFS，不读写 SD 卡。
- 在真实 AF12 切换前继续评估 OV5640 DVP 输出与 SDIO 信号方向之间的物理冲突风险。

### 7. Stage 11B-10 板测结果

#### 7.1 5 轮循环测试

执行命令：

```text
python tools/uart_snapshot_takeover_cycle_test.py --cycles 5 --tag stage11_b10_5cycle
```

测试统计：

```text
cycle_total=5
prepare_ok_count=5
takeover_enter_ok_count=5
text_dump_block_ok_count=5
binary_block_ok_count=5
takeover_exit_ok_count=5
restore_command_ok_count=5
restore_binary_ok_count=5
fail_count=0
first_frame_id=1
last_frame_id=5
frame_id_continuous=是
avg_restore_binary_time_ms=3454.20
min_restore_binary_time_ms=3447.52
max_restore_binary_time_ms=3460.37
测试结果=PASS
```

输出文件：

```text
captures\snapshot_takeover_cycle_stage11_b10_5cycle_20260805_151818.csv
captures\snapshot_takeover_cycle_stage11_b10_5cycle_20260805_151818_summary.txt
```

#### 7.2 20 轮循环测试

执行命令：

```text
python tools/uart_snapshot_takeover_cycle_test.py --cycles 20 --tag stage11_b10_20cycle
```

测试统计：

```text
cycle_total=20
prepare_ok_count=20
takeover_enter_ok_count=20
text_dump_block_ok_count=20
binary_block_ok_count=20
takeover_exit_ok_count=20
restore_command_ok_count=20
restore_binary_ok_count=20
fail_count=0
first_frame_id=6
last_frame_id=25
frame_id_continuous=是
avg_restore_binary_time_ms=3463.90
min_restore_binary_time_ms=3446.63
max_restore_binary_time_ms=3478.61
测试结果=PASS
```

输出文件：

```text
captures\snapshot_takeover_cycle_stage11_b10_20cycle_20260805_151858.csv
captures\snapshot_takeover_cycle_stage11_b10_20cycle_20260805_151858_summary.txt
```

#### 7.3 合计 25 轮结果

5 轮和 20 轮测试均通过，合计完成 25 轮完整流程：

```text
SNAPSHOT PREPARE
SD TAKEOVER ENTER
guard 文本 DUMP 阻止
guard binary 阻止
SD TAKEOVER EXIT
SNAPSHOT RESTORE
RESTORE 后 binary PASS
```

合计结果：

- `SNAPSHOT PREPARE` 成功 25 次。
- `SD TAKEOVER ENTER` 成功检测到 `conflict pins released` 25 次。
- guard 状态下文本 DUMP 被阻止 25 次。
- guard 状态下 binary 请求被阻止 25 次。
- `SD TAKEOVER EXIT` 成功检测到 `conflict pins restored` 25 次。
- `SNAPSHOT RESTORE` 响应正常 25 次。
- RESTORE 后 binary 请求成功 25 次。
- RESTORE 后 `frame_id` 从 1 到 25 连续递增。
- 测试期间未观察到复位、FATAL 或 COM4 占用。

### 8. 最终 SD TAKEOVER STATUS

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=25
takeover_exit_attempt_count=25
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
takeover_precheck_required=1
takeover_precheck_attempt_count=25
takeover_precheck_success_count=25
takeover_precheck_fail_count=0
snapshot_pause_required=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_takeover_precheck_error_code=0
last_takeover_precheck_error_text=OK
conflict_pin_release_attempt_count=25
conflict_pin_release_success_count=25
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=25
conflict_pin_restore_success_count=25
conflict_pin_restore_error_count=0
conflict_pins_released=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
last_conflict_pin_operation_ms=0
```

`conflict_pin_release_success_count=25` 表明 PC8、PC9、PC11 连续释放成功 25 次；`conflict_pin_restore_success_count=25` 表明三个引脚连续恢复为 DCMI AF13 成功 25 次，释放和恢复错误计数均为 0。最终 `conflict_pins_released=0` 表明引脚已回到非释放状态。`last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED` 属于预期，因为本阶段仍未实现真实 SDIO 初始化。`last_conflict_pin_operation_ms=0` 可以接受，GPIO 复用操作在当前 tick 分辨率内可能显示为 0 ms。

### 9. 最终 SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=25
restore_attempt_count=25
prepare_success_count=25
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=25
dcmi_stop_success_count=25
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=25
binary_block_count=25
```

`dcmi_stop_success_count=25` 表明 `HAL_DCMI_Stop` 多轮调用稳定。最终 `software_guard_active=0`、`dump_block_required=0` 表明 RESTORE 后 guard 已清除。`dump_block_count=25` 和 `binary_block_count=25` 是预期结果，对应每轮 guard 状态下各阻止一次。

### 10. 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=75
dump_success_count=25
dump_error_count=50
binary_request_count=50
binary_request_success_count=25
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=40
last_binary_error_code=0
last_error_code=8
```

统计解释：

- `dump_request_count=75` 是预期结果，对应每轮 1 次 guard 文本 DUMP、1 次 guard binary 请求和 1 次 RESTORE 后 binary 请求。
- `dump_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 图像导出成功。
- `dump_error_count=50` 是预期结果，对应 25 次 guard 文本 DUMP 阻止和 25 次 guard binary 阻止。
- `binary_request_count=50` 是预期结果，对应 25 次 guard binary 请求和 25 次 RESTORE 后 binary 请求。
- `binary_request_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 请求成功。
- `binary_request_error_count=0` 是正确结果，因为 guard 状态下二进制请求格式正确，只是被 snapshot guard 拦截，不属于协议错误。
- `last_error_code=8` 对应 snapshot guard active 类错误，属于 guard 阻止路径的预期记录。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7592
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=37
monitor_heartbeat_age_ms=2
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=153
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=221
uart_dma_rx_bytes=2728
stream_buffer_write_bytes=2728
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 11. Stage 11B-10 板测结论与适用边界

Stage 11B-10 验证通过。新增 `uart_snapshot_takeover_cycle_test.py` 后，5 轮和 20 轮多轮冲突引脚释放/恢复循环测试均 PASS。合计 25 轮测试中，`SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、guard 文本 DUMP 阻止、guard binary 阻止、`SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、RESTORE 后 binary 图像恢复均正常；RESTORE 后 `frame_id` 从 1 到 25 连续递增。

最终 SD TAKEOVER STATUS 显示 PC8、PC9、PC11 释放成功 25 次、恢复成功 25 次、错误 0 次，最终 `conflict_pins_released=0`。最终 STATUS 显示 IWDG 未跳过喂狗、Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明多轮 PC8、PC9、PC11 释放/恢复闭环稳定。

需要严谨说明：本轮只验证了 PC8、PC9、PC11 在 DCMI AF13 与 GPIO 输入态之间的多轮释放/恢复稳定性。虽然 RESTORE 后图像导出持续恢复正常，但本轮没有配置 `GPIO_AF12_SDIO`，没有初始化 SDIO 或 FATFS，也没有访问 SD 卡。因此本轮通过不能说明真实 SD 卡接管已经完成。

### 12. 后续 Stage 11B-11 建议

Stage 11B-11 建议进入“真实 SDIO AF12 切换但不初始化 SD 卡”的最小验证。目标是验证 PC8、PC9、PC11 能完成以下安全闭环：

```text
DCMI AF13 -> GPIO 输入态 -> SDIO AF12 -> GPIO 输入态 -> DCMI AF13
```

Stage 11B-11 仍不调用 `HAL_SD_Init`，不接入 FATFS，不读写 SD 卡；应先验证纯 GPIO 复用闭环和 RESTORE 后图像恢复，再考虑真实 SD 卡初始化。

## Stage 11B-11 SDIO AF12切换但不初始化SD卡

### 1. 本轮目的

- `SNAPSHOT PREPARE` 后，`SD TAKEOVER ENTER` 先将 PC8、PC9、PC11 从 DCMI AF13 释放为 GPIO 输入态，再切换为 `GPIO_AF12_SDIO`。
- `SD TAKEOVER EXIT` 先将 PC8、PC9、PC11 从 SDIO AF12 退回 GPIO 输入态，再恢复为 DCMI AF13。
- 验证在不初始化 SDIO/FATFS 的情况下，以下纯 GPIO 复用闭环不会破坏 RESTORE 后的图像导出：

  ```text
  DCMI AF13 -> GPIO 输入态 -> SDIO AF12 -> GPIO 输入态 -> DCMI AF13
  ```

`conflict_pins_released` 仅表示三个冲突引脚当前处于 GPIO 输入释放态。切换到 SDIO AF12 后该字段为 0，并由 `sdio_af12_selected=1` 表示当前复用状态。

### 2. 本轮不做

- 不调用 `HAL_SD_Init`、`HAL_SD_ConfigWideBusOperation`、`HAL_SD_ReadBlocks` 或 `HAL_SD_WriteBlocks`。
- 不读写 SD 卡，不接入 FATFS。
- 不配置 PC10、PC12、PD2，不形成完整 SDIO 总线。
- 不调用 `HAL_DCMI_Start_DMA`，不调用 `HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不重新初始化 OV5640，不重新初始化 DCMI 外设。
- 不修改 UART DMA、二进制请求帧格式、OV56RGB5 图像帧格式、IWDG、FreeRTOS 任务优先级或任务栈大小。

### 3. 新增状态字段

- `sdio_af12_switch_attempt_count`：尝试将 PC8、PC9、PC11 切换到 SDIO AF12 的次数。
- `sdio_af12_switch_success_count`：SDIO AF12 切换成功次数。
- `sdio_af12_switch_error_count`：SDIO AF12 切换失败次数。
- `sdio_af12_restore_attempt_count`：尝试从 SDIO AF12 退回 GPIO 输入态的次数。
- `sdio_af12_restore_success_count`：从 SDIO AF12 退回 GPIO 输入态成功次数。
- `sdio_af12_restore_error_count`：从 SDIO AF12 退回 GPIO 输入态失败次数。
- `sdio_af12_selected`：PC8、PC9、PC11 当前是否处于 SDIO AF12 复用状态。
- `last_sdio_af12_error_code`：最近一次 SDIO AF12 切换或退出错误码。
- `last_sdio_af12_operation_ms`：最近一次 SDIO AF12 切换或退出的实际处理耗时，按出口 tick 减入口 tick 记录。

`SD STATUS` 和 `SD TAKEOVER STATUS` 同时输出以上字段，并补充 `last_sdio_af12_error_text` 文本字段。新增错误码为 `SDIO_AF12_SWITCH_FAILED` 和 `SDIO_AF12_RESTORE_FAILED`。

### 4. 预期命令行为

- 未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 保持被阻止，不释放冲突引脚，也不切换 SDIO AF12。
- `SNAPSHOT PREPARE` 后，`SD TAKEOVER ENTER` 释放 PC8、PC9、PC11 并使用推挽复用、上拉、超高速和 AF12 配置切换到 SDIO 引脚状态。
- AF12 切换成功后进入 `ENTER_DEFERRED`，返回 `TAKEOVER_NOT_IMPLEMENTED`；该返回值表示 SDIO 外设、SD 卡和 FATFS 尚未初始化，并非 GPIO 切换失败。
- `SD TAKEOVER EXIT` 在 `sdio_af12_selected=1` 时先退回 GPIO 输入态，再调用既有恢复流程配置为 DCMI AF13；未处于 AF12 时仍允许直接恢复 DCMI AF13。
- EXIT 成功后 `conflict_pins_released=0`、`sdio_af12_selected=0`，状态进入 `EXIT_DEFERRED`，仍返回 `TAKEOVER_NOT_IMPLEMENTED`。
- `SNAPSHOT RESTORE` 后，`basic`、`pc_dump`、`repeat` 应恢复正常。
- `SD INIT` 仍保持 `NEED_TAKEOVER`，即使三个冲突引脚已切换到 AF12，也不会继续执行 SDIO 初始化。

### 5. 风险说明

- 本轮开始真实配置 `GPIO_AF12_SDIO`，但没有初始化 SDIO 外设，因此不会真正访问 SD 卡。
- 本轮只处理 PC8、PC9、PC11，没有处理 PC10、PC12、PD2，SDIO 数据、时钟和命令线并不完整。
- OV5640 传感器本身可能仍在输出 DVP 数据，本轮仍未处理传感器 PWDN 或寄存器停流。
- GPIO 配置完成后通过 GPIOC 的 MODER、PUPDR、OSPEEDR、OTYPER 和 AFR[1] 做最小回读验证；回读失败时进入 ERROR，不继续任何 SDIO 初始化。
- 后续真实初始化 SDIO 前，仍需谨慎处理全部 SDIO 引脚、初始化阶段不超过 400 kHz 的时钟、CMD 线和总线状态。

### 6. 板测计划

1. 确认启动正常，无 FATAL、反复复位或 IWDG 复位循环。
2. 未执行 `SNAPSHOT PREPARE` 时运行 `SD TAKEOVER ENTER`，确认返回 blocked 且 AF12 切换计数不增加。
3. 执行 `SNAPSHOT PREPARE`，确认相机暂停且软件 guard 生效。
4. 执行 `SD TAKEOVER ENTER`，确认 PC8、PC9、PC11 切换到 SDIO AF12，`sdio_af12_selected=1`。
5. 确认 guard 状态下文本 DUMP 和 binary 请求均被阻止。
6. 执行 `SD TAKEOVER EXIT`，确认先退出 AF12，再恢复 DCMI AF13，`sdio_af12_selected=0`。
7. 执行 `SNAPSHOT RESTORE`，确认 guard 清除。
8. 执行 `SD INIT`，确认仍返回 `NEED_TAKEOVER`。
9. 执行 `basic`、`pc_dump` 和 `repeat 20`，确认图像导出恢复且 frame_id 连续。
10. 检查 STATUS 中 IWDG、Hook、UART RX DMA、堆栈和协议统计均正常。

本轮 Codex 不执行硬件测试；烧录、串口命令和图像回归由用户在开发板上完成。

### 7. 后续 Stage 11B-12 建议

- 如果本轮单次切换验证通过，下一步执行多轮 SDIO AF12 切换循环稳定性验证。
- Stage 11B-12 仍不调用 `HAL_SD_Init`，仍不接入 FATFS，不读写 SD 卡。
- 重点验证多次 `DCMI AF13 -> GPIO 输入态 -> SDIO AF12 -> GPIO 输入态 -> DCMI AF13` 后，RESTORE 图像链路、IWDG、Hook 和 UART RX DMA 是否持续稳定。

### 8. Stage 11B-11 板测结果

#### 8.1 启动情况

- 启动正常。
- `reset: iwdg=0`。
- `OV5640 ID = 0x5640`。
- `Camera init OK`。
- 无 FATAL。
- 无反复复位。
- 无 IWDG 复位循环。

#### 8.2 未执行 SNAPSHOT PREPARE 时的前置条件保护

直接执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

`SD TAKEOVER STATUS` 结果：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=0
takeover_precheck_fail_count=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=0
sdio_af12_selected=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
last_takeover_precheck_error_code=9
last_takeover_precheck_error_text=SNAPSHOT_NOT_PAUSED
```

结果符合预期：未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 被正确阻止；PC8、PC9、PC11 没有被释放，也没有切换到 SDIO AF12。

#### 8.3 SNAPSHOT PREPARE

执行 `SNAPSHOT PREPARE`，输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

状态结果：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

`HAL_DCMI_Stop` 成功，相机控制状态进入 `CAMERA_PAUSED`，软件 guard 和 DUMP 阻止条件均已生效。

#### 8.4 PREPARE 后切换 PC8、PC9、PC11 到 SDIO AF12

执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: conflict pins switched to SDIO AF12, SD init is not implemented yet.
```

`SD TAKEOVER STATUS` 结果：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=2
takeover_precheck_attempt_count=2
takeover_precheck_success_count=1
takeover_precheck_fail_count=1
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=1
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

PC8、PC9、PC11 已先释放为 GPIO 输入态，随后成功切换为 `GPIO_AF12_SDIO`。`conflict_pins_released=0` 是合理结果，因为此时三个引脚已经不是 GPIO 输入释放态，而是 SDIO AF12 复用态；当前复用状态由 `sdio_af12_selected=1` 表示。

返回 `TAKEOVER_NOT_IMPLEMENTED` 属于预期行为：本阶段只完成 GPIO 复用切换，仍未调用 `HAL_SD_Init`，也未接入 FATFS。

#### 8.5 AF12 状态下的 SD INIT 边界验证

在 `sdio_af12_selected=1` 时执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

SD 状态：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
takeover_state_text=ENTER_DEFERRED
sdio_af12_selected=1
```

即使 PC8、PC9、PC11 已切换到 SDIO AF12，`SD INIT` 仍保持 `NEED_TAKEOVER`：本轮没有真实初始化 SDIO，没有接入 FATFS，没有读写 SD 卡，也没有配置 PC10、PC12、PD2。

本次记录的 `last_operation_ms=57447` 仍更像系统绝对 tick，而不是本次命令处理耗时。这属于既有耗时统计问题，不影响 Stage 11B-11 的 AF12 切换验证结论，后续应单独修正。

#### 8.6 guard 状态下 DUMP 和 binary 阻止验证

文本 DUMP 输出：

```text
DUMP blocked: snapshot software guard active.
```

- 未发送 OV56RGB5 图像帧。
- `dump_block_count=1`。
- guard 状态下执行 binary basic，响应长度为 0 B，PC 端接收超时，测试结果为 FAIL。
- 此处 FAIL 是软件 guard 生效后的预期现象。
- `binary_block_count=1`。

#### 8.7 SD TAKEOVER EXIT 恢复 DCMI AF13

执行 `SD TAKEOVER EXIT`，输出：

```text
SD TAKEOVER EXIT: conflict pins restored to DCMI AF13 from SDIO AF12.
```

状态结果：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
sdio_af12_restore_attempt_count=1
sdio_af12_restore_success_count=1
sdio_af12_restore_error_count=0
sdio_af12_selected=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
```

PC8、PC9、PC11 已从 SDIO AF12 退出，并恢复为 DCMI AF13。本轮仍未调用 `HAL_DCMI_Start_DMA`，也未重新初始化 OV5640 或 DCMI 外设。

#### 8.8 SNAPSHOT RESTORE 与前置条件复位

执行 `SNAPSHOT RESTORE`，输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

状态结果：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

RESTORE 后再次执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

此时状态为：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=3
takeover_precheck_attempt_count=3
takeover_precheck_success_count=1
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pins_released=0
sdio_af12_selected=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
last_takeover_precheck_error_code=9
last_takeover_precheck_error_text=SNAPSHOT_NOT_PAUSED
```

`SNAPSHOT RESTORE` 后接管前置条件重新失效，说明 AF12 切换流程退出后，软件安全门状态已恢复正常。

#### 8.9 RESTORE 后图像功能回归

- 第一次 guard 状态下 binary basic 响应长度为 0 B、结果 FAIL，属于预期阻止现象。
- RESTORE 后 basic：PASS，`frame_id=1`。
- RESTORE 后 pc_dump：PASS，`frame_id=2`，图像质量无警告。
- RESTORE 后 repeat：20/20 PASS。
- repeat 的 `frame_id` 从 3 到 22 连续递增。
- repeat 平均耗时 3466.07 ms。
- repeat 最短耗时 3453.78 ms。
- repeat 最长耗时 3469.20 ms。

#### 8.10 最终 SD TAKEOVER STATUS

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=3
takeover_exit_attempt_count=1
takeover_error_count=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
takeover_precheck_attempt_count=3
takeover_precheck_success_count=1
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_restore_attempt_count=1
sdio_af12_restore_success_count=1
sdio_af12_restore_error_count=0
sdio_af12_selected=0
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
```

最终状态表明 AF12 切换与退出各成功一次、错误计数均为 0，三个冲突引脚最终不处于输入释放态或 SDIO AF12 状态。最终 `takeover_state=IDLE` 和 `SNAPSHOT_NOT_PAUSED` 来自 RESTORE 后再次执行 ENTER 的预期前置条件阻止。

#### 8.11 最终 SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

`restore_success_count=0` 和 `CAMERA_RESTORE_NOT_IMPLEMENTED` 符合当前阶段边界：RESTORE 负责清除软件 guard，但尚未实现 DCMI 硬件重启；现有图像请求链路在后续回归中仍可恢复工作。

#### 8.12 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

统计解释：

- `dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 binary 图像请求各被阻止一次。
- `binary_request_error_count=0` 是正确结果，因为二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。
- `last_error_code=8` 对应 snapshot guard active 类错误，属于本轮 guard 阻止路径的预期记录。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7552
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=1
monitor_heartbeat_age_ms=620
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=880
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=53
uart_dma_rx_bytes=647
stream_buffer_write_bytes=647
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

#### 8.13 板测结论

Stage 11B-11 验证通过。未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 被正确阻止，且未释放冲突引脚、未切换 SDIO AF12。`SNAPSHOT PREPARE` 成功后，`SD TAKEOVER ENTER` 能将 PC8、PC9、PC11 先释放为 GPIO 输入态，再切换为 `GPIO_AF12_SDIO`，并记录 `sdio_af12_selected=1`。

AF12 状态下 `SD INIT` 仍保持 `NEED_TAKEOVER`，没有初始化 SDIO 或 FATFS。`SD TAKEOVER EXIT` 能将 PC8、PC9、PC11 从 SDIO AF12 退出，并恢复为 DCMI AF13，最终记录 `sdio_af12_selected=0`。随后 `SNAPSHOT RESTORE` 清除 guard，RESTORE 后 basic、pc_dump、repeat 均恢复正常。

最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明本轮单次 SDIO AF12 GPIO 复用切换闭环没有破坏现有摄像头采集、DUMP、二进制请求和运行保护机制。

#### 8.14 适用边界与严谨说明

本轮虽然真实配置了 PC8、PC9、PC11 的 `GPIO_AF12_SDIO`，但没有配置 PC10、PC12、PD2，没有初始化 SDIO 外设，没有调用 `HAL_SD_Init`，也没有接入 FATFS 或读写 SD 卡。因此，本轮通过只能说明 MCU 侧部分 SDIO 数据线的 AF12 切换闭环可用，不能说明 SD 卡通信已经可用。

#### 8.15 后续 Stage 11B-12 建议

Stage 11B-12 建议新增 PC 端自动化脚本，执行多轮以下完整流程：

```text
SNAPSHOT PREPARE
SD TAKEOVER ENTER
SD INIT 保持 NEED_TAKEOVER
DUMP_BLOCK
BINARY_BLOCK
SD TAKEOVER EXIT
SNAPSHOT RESTORE
RESTORE_BINARY
```

Stage 11B-12 仍不调用 `HAL_SD_Init`，不接入 FATFS；先验证多轮 SDIO AF12 切换闭环、guard 阻止路径和 RESTORE 后图像恢复的稳定性。

## Stage 11B-12 多轮 SDIO AF12 切换闭环稳定性验证

### 1. 本轮目的

- 新增独立 PC 端测试工具 `tools/uart_snapshot_sdio_af12_cycle_test.py`。
- 自动执行多轮 `SNAPSHOT PREPARE / SD TAKEOVER ENTER / SD INIT / DUMP_BLOCK / BINARY_BLOCK / SD TAKEOVER EXIT / SNAPSHOT RESTORE / RESTORE_BINARY` 完整流程。
- 验证 PC8、PC9、PC11 多次切换到 SDIO AF12、再退出 AF12 并恢复为 DCMI AF13 后，图像请求链路是否持续稳定。
- 验证 AF12 状态下 `SD INIT` 始终保持 `NEED_TAKEOVER`，不会真实初始化 SDIO 或 FATFS。
- 验证 snapshot guard 生效期间，文本 DUMP 和 binary 图像请求持续被阻止。
- 验证 `SNAPSHOT RESTORE` 后 binary 图像请求持续恢复，并检查 `frame_id` 连续递增。

### 2. 本轮不做

- 不修改任何固件 C/H 源码，不修改 Core 或 BSPDrivers。
- 不新增 `HAL_DCMI_Stop`，不调用 `HAL_DCMI_Start_DMA`、`HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不修改 PC8、PC9、PC11 的 GPIO/AF 切换固件实现，不新增 `GPIO_AF12_SDIO` 固件代码。
- 不配置 PC10、PC12、PD2。
- 不初始化 SDIO，不调用任何 HAL SD API。
- 不接入 FATFS，不读写 SD 卡。
- 不修改 UART/二进制图像请求协议，也不修改任何现有 Python 工具。

### 3. 测试脚本

脚本路径：

```text
tools/uart_snapshot_sdio_af12_cycle_test.py
```

默认参数：

```text
port=COM4
baud=115200
cycles=5
guard_timeout=2.0 s
frame_timeout=10.0 s
interval=0.2 s
tag=stage11_b12_sdio_af12_cycle
DTR=False
RTS=False
```

脚本只依赖 pyserial 和 Python 标准库，不导入现有项目脚本，不使用类、线程或 async，不保存 PNG，也不做图像质量分析。

每轮按以下顺序执行：

1. `SNAPSHOT PREPARE` 必须包含 `DCMI stop OK`。
2. `SD TAKEOVER ENTER` 必须包含 `conflict pins switched to SDIO AF12`、`sdio_af12_selected=1` 和 AF12 切换成功计数字段。
3. `SD INIT` 必须包含 `NEED_TAKEOVER`、`is_initialized=0`、`sdio_ready=0`、`fatfs_ready=0` 和 `sdio_af12_selected=1`。
4. 文本 `DUMP` 必须被 snapshot guard 阻止。
5. guard 状态下发送 binary 图像请求，不得收到合法 OV56RGB5 图像帧；0 B、超时或无合法 magic 均属于预期。
6. `SD TAKEOVER EXIT` 必须包含 `conflict pins restored to DCMI AF13 from SDIO AF12`、`sdio_af12_selected=0`、AF12 退出成功计数和 DCMI AF13 恢复成功计数。
7. `SNAPSHOT RESTORE` 响应必须包含 `SNAPSHOT RESTORE` 或 `RESTORE_DEFERRED`。
8. RESTORE 后 binary 请求必须收到合法 OV56RGB5 图像帧，并通过 version、pixel format、160x120 尺寸、38400 B payload 和 CRC32 校验。

每轮输出格式为：

```text
[01/05] PREPARE=PASS AF12_ENTER=PASS SD_INIT_DEFERRED=PASS DUMP_BLOCK=PASS BINARY_BLOCK=PASS AF12_EXIT=PASS RESTORE=PASS RESTORE_BINARY=PASS frame_id=xx time=xxxx ms
```

单项失败时，脚本打印失败原因、写入本轮 CSV，默认继续后续步骤和下一轮；只有串口打开失败时直接结束。

### 4. 协议和结果校验

脚本自行构造 14 字节二进制图像请求：请求 magic 为 `A5 5A`，version 为 1，type 为 `0x20`，seq 为小端 uint16，len 固定为 0，对 version/type/seq/len 六字节计算 CRC32，结尾为 `0D 0A`。

RESTORE 后响应必须是总长 38426 B 的 OV56RGB5 图像帧：22 B header、38400 B RGB565 payload 和 4 B CRC。脚本检查 magic、version、pixel format、width、height、payload_len 和 payload CRC，并使用 `frame_id` 判断跨轮图像是否逐帧加一。

### 5. 测试统计与输出文件

脚本统计以下字段：

- `cycle_total`
- `prepare_ok_count`
- `takeover_enter_ok_count`
- `sd_init_deferred_ok_count`
- `text_dump_block_ok_count`
- `binary_block_ok_count`
- `takeover_exit_ok_count`
- `restore_command_ok_count`
- `restore_binary_ok_count`
- `fail_count`
- `first_frame_id`
- `last_frame_id`
- `frame_id_continuous`
- `avg_restore_binary_time_ms`
- `min_restore_binary_time_ms`
- `max_restore_binary_time_ms`

脚本自动创建 `captures` 目录并生成：

```text
captures/sdio_af12_cycle_<tag>_<timestamp>.csv
captures/sdio_af12_cycle_<tag>_<timestamp>_summary.txt
```

CSV 保存每轮八项结果、`frame_id`、RESTORE 后图像请求耗时和错误原因。summary 保存测试参数、全部统计、`frame_id` 连续性、最终 PASS/FAIL，并明确说明 guard binary 超时是预期现象、ENTER 后必须处于 AF12、`SD INIT` 必须保持 `NEED_TAKEOVER`、EXIT 后必须恢复 DCMI AF13、RESTORE 后 binary 必须 PASS。

总测试只有在每轮八项检查全部通过、`fail_count=0`、RESTORE 后成功帧数量等于循环次数且 `frame_id` 连续递增时才判定为 PASS。

### 6. 后续板测计划

1. 先执行 5 轮：

   ```text
   python tools/uart_snapshot_sdio_af12_cycle_test.py --cycles 5 --tag stage11_b12_5cycle
   ```

2. 5 轮通过后再执行 20 轮：

   ```text
   python tools/uart_snapshot_sdio_af12_cycle_test.py --cycles 20 --tag stage11_b12_20cycle
   ```

3. 最后通过串口依次执行：

   ```text
   SD TAKEOVER STATUS
   SNAPSHOT STATUS
   STATUS
   ```

4. 确认最终 `sdio_af12_selected=0`，AF12 切换/退出和冲突引脚恢复计数符合循环次数，错误计数均为 0。
5. 检查 IWDG、Hook、UART RX DMA、StreamBuffer、任务心跳、堆栈和图像请求统计。
6. 若 20 轮全部通过且运行保护状态正常，再进入下一阶段。

本轮 Codex 只进行脚本静态编译检查，不打开 COM4，也不执行硬件测试；循环结果由用户在开发板上验证并回填。

### 7. 后续 Stage 11C 建议

- 如果 Stage 11B-12 多轮验证稳定，可认为 Stage 11B 已完成“SDIO 接管前 GPIO/AF 切换安全闭环验证”。
- 下一阶段进入 Stage 11C：SDIO 最小初始化验证。
- Stage 11C 应先只验证 SDIO 初始化和 SD 卡信息读取，不立即接入 FATFS 写文件。
- Stage 11C 必须继续保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序。
- 真实 SDIO 初始化前仍需配置并检查 PC10、PC12、PD2，严格控制初始化阶段 SDIO_CK 不超过 400 kHz，并制定失败路径下退出 AF12、恢复 DCMI AF13 和清除 guard 的闭环。

### 8. Stage 11B-12 板测结果

#### 8.1 5 轮循环测试

执行命令：

```text
python tools/uart_snapshot_sdio_af12_cycle_test.py --cycles 5 --tag stage11_b12_5cycle
```

测试结果：

```text
cycle_total=5
prepare_ok_count=5
takeover_enter_ok_count=5
sd_init_deferred_ok_count=5
text_dump_block_ok_count=5
binary_block_ok_count=5
takeover_exit_ok_count=5
restore_command_ok_count=5
restore_binary_ok_count=5
fail_count=0
first_frame_id=1
last_frame_id=5
frame_id_continuous=是
avg_restore_binary_time_ms=3470.54
min_restore_binary_time_ms=3461.26
max_restore_binary_time_ms=3489.82
测试结果=PASS
```

输出文件：

```text
captures\sdio_af12_cycle_stage11_b12_5cycle_20260805_162534.csv
captures\sdio_af12_cycle_stage11_b12_5cycle_20260805_162534_summary.txt
```

#### 8.2 20 轮循环测试

执行命令：

```text
python tools/uart_snapshot_sdio_af12_cycle_test.py --cycles 20 --tag stage11_b12_20cycle
```

测试结果：

```text
cycle_total=20
prepare_ok_count=20
takeover_enter_ok_count=20
sd_init_deferred_ok_count=20
text_dump_block_ok_count=20
binary_block_ok_count=20
takeover_exit_ok_count=20
restore_command_ok_count=20
restore_binary_ok_count=20
fail_count=0
first_frame_id=6
last_frame_id=25
frame_id_continuous=是
avg_restore_binary_time_ms=3459.91
min_restore_binary_time_ms=3448.30
max_restore_binary_time_ms=3472.37
测试结果=PASS
```

输出文件：

```text
captures\sdio_af12_cycle_stage11_b12_20cycle_20260805_162651.csv
captures\sdio_af12_cycle_stage11_b12_20cycle_20260805_162651_summary.txt
```

#### 8.3 合计 25 轮结果

5 轮和 20 轮测试均通过，合计完成 25 轮以下完整流程：

```text
SNAPSHOT PREPARE
SD TAKEOVER ENTER
SD INIT
guard 文本 DUMP 阻止
guard binary 阻止
SD TAKEOVER EXIT
SNAPSHOT RESTORE
RESTORE 后 binary PASS
```

合计结果：

- `SNAPSHOT PREPARE` 成功 25 次。
- `SD TAKEOVER ENTER` 成功检测到 `conflict pins switched to SDIO AF12` 25 次。
- `SD INIT` 保持 `NEED_TAKEOVER` 25 次。
- guard 状态下文本 DUMP 被阻止 25 次。
- guard 状态下 binary 请求被阻止 25 次。
- `SD TAKEOVER EXIT` 成功检测到 `conflict pins restored to DCMI AF13 from SDIO AF12` 25 次。
- `SNAPSHOT RESTORE` 响应正常 25 次。
- RESTORE 后 binary 请求成功 25 次。
- RESTORE 后 `frame_id` 从 1 到 25 连续递增。
- 测试期间未观察到复位、FATAL 或 COM4 占用。

#### 8.4 最终 SD TAKEOVER STATUS

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=25
takeover_exit_attempt_count=25
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
takeover_precheck_attempt_count=25
takeover_precheck_success_count=25
takeover_precheck_fail_count=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=25
conflict_pin_release_success_count=25
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=25
conflict_pin_restore_success_count=25
conflict_pin_restore_error_count=0
conflict_pins_released=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
sdio_af12_switch_attempt_count=25
sdio_af12_switch_success_count=25
sdio_af12_switch_error_count=0
sdio_af12_restore_attempt_count=25
sdio_af12_restore_success_count=25
sdio_af12_restore_error_count=0
sdio_af12_selected=0
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
```

状态解释：

- `sdio_af12_switch_success_count=25` 表明 PC8、PC9、PC11 切换到 SDIO AF12 成功 25 次。
- `sdio_af12_restore_success_count=25` 表明 PC8、PC9、PC11 从 SDIO AF12 退出成功 25 次。
- `conflict_pin_restore_success_count=25` 表明 PC8、PC9、PC11 恢复为 DCMI AF13 成功 25 次。
- `sdio_af12_selected=0` 和 `conflict_pins_released=0` 表明最终已退出 AF12，并恢复到非输入释放状态。
- `last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED` 是预期现象，因为本阶段仍未实现真实 SDIO 初始化。
- `last_sdio_af12_operation_ms=0` 可以接受，GPIO 复用切换很快，在当前 tick 分辨率下可能显示为 0 ms。

#### 8.5 最终 SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=25
restore_attempt_count=25
prepare_success_count=25
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=25
dcmi_stop_success_count=25
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=25
binary_block_count=25
```

`dcmi_stop_success_count=25` 表明 `HAL_DCMI_Stop` 多轮调用稳定。最终 `software_guard_active=0`、`dump_block_required=0` 表明 RESTORE 后 guard 已清除。`dump_block_count=25` 和 `binary_block_count=25` 是预期结果，对应每轮 guard 状态下各阻止一次。

#### 8.6 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=75
dump_success_count=25
dump_error_count=50
binary_request_count=50
binary_request_success_count=25
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=40
last_binary_error_code=0
last_error_code=8
```

统计解释：

- `dump_request_count=75` 是预期结果，对应每轮 1 次 guard 文本 DUMP、1 次 guard binary 请求和 1 次 RESTORE 后 binary 请求。`SD INIT` 不计入 `dump_request_count`；此前将其考虑在该计数内的预估不严谨，本轮实际值 75 正确。
- `dump_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 图像导出成功。
- `dump_error_count=50` 是预期结果，对应 25 次 guard 文本 DUMP 阻止和 25 次 guard binary 阻止。
- `binary_request_count=50` 是预期结果，对应 25 次 guard binary 请求和 25 次 RESTORE 后 binary 请求。
- `binary_request_success_count=25` 是预期结果，对应 RESTORE 后 25 次 binary 请求成功。
- `binary_request_error_count=0` 是正确结果，因为 guard 状态下二进制请求格式正确，只是被 snapshot guard 拦截，不属于协议错误。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7552
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=75
monitor_heartbeat_age_ms=38
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=191
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=249
uart_dma_rx_bytes=2945
stream_buffer_write_bytes=2945
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

#### 8.7 板测结论与适用边界

Stage 11B-12 验证通过。新增 `uart_snapshot_sdio_af12_cycle_test.py` 后，5 轮和 20 轮多轮 SDIO AF12 切换闭环测试均 PASS。合计 25 轮测试中，`SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、`SD INIT deferred`、guard 文本 DUMP 阻止、guard binary 阻止、`SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、RESTORE 后 binary 图像恢复均正常；RESTORE 后 `frame_id` 从 1 到 25 连续递增。

最终 SD TAKEOVER STATUS 显示 PC8、PC9、PC11 切换到 SDIO AF12 成功 25 次、从 SDIO AF12 退出成功 25 次、恢复为 DCMI AF13 成功 25 次，错误均为 0，最终 `sdio_af12_selected=0`、`conflict_pins_released=0`。最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明多轮 SDIO AF12 切换闭环稳定。

需要严谨说明：本轮只验证了 PC8、PC9、PC11 在 DCMI AF13、GPIO 输入态和 SDIO AF12 之间的多轮切换稳定性。虽然 AF12 状态下 `SD INIT` 保持 `NEED_TAKEOVER`，且 RESTORE 后图像导出持续恢复正常，但本轮没有配置 PC10、PC12、PD2，没有初始化 SDIO 外设，没有调用 `HAL_SD_Init`，没有接入 FATFS，也没有访问 SD 卡。因此，本轮通过不能说明真实 SD 卡通信已经可用。

#### 8.8 后续 Stage 11C 建议

Stage 11B 到此可以认为完成“SDIO 接管前 GPIO/AF 切换安全闭环验证”。下一阶段进入 Stage 11C：SDIO 最小初始化验证，建议继续拆分为：

- Stage 11C-1：SDIO 全引脚 AF12 切换骨架，只配置 PC8、PC9、PC10、PC11、PC12、PD2，不初始化 SDIO。
- Stage 11C-2：`HAL_SD_Init` 最小初始化，不接入 FATFS。
- Stage 11C-3：读取 SD 卡基础信息，不写卡。
- Stage 11C-4：多轮 SD 初始化/退出稳定性验证。
- Stage 11C-5：再考虑 FATFS mount 和最小文件写入。

Stage 11C 必须继续保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序，并为任一初始化失败路径保留退出 SDIO、恢复 DCMI AF13 和清除 guard 的闭环。

## Stage 11C-1 SDIO全引脚AF12切换骨架

### 1. 本轮目的

- `SNAPSHOT PREPARE` 后，`SD TAKEOVER ENTER` 继续先将 PC8、PC9、PC11 从 DCMI AF13 释放为 GPIO 输入态。
- 沿用既有冲突引脚切换流程后，将 PC8、PC9、PC10、PC11、PC12、PD2 全部配置为 `GPIO_AF12_SDIO`。
- `SD TAKEOVER EXIT` 将六个 SDIO 引脚从 AF12 退回无上下拉的 GPIO 输入态，再将 PC8、PC9、PC11 恢复为 DCMI AF13。
- EXIT 后 PC10、PC12、PD2 保持 GPIO 输入态。
- 验证在不初始化 SDIO/FATFS 的情况下，完整 SDIO GPIO AF12 切换闭环不会破坏 RESTORE 后图像导出。

完整 GPIO 复用闭环为：

```text
PC8/PC9/PC11: DCMI AF13 -> GPIO 输入态 -> SDIO AF12 -> GPIO 输入态 -> DCMI AF13
PC10/PC12/PD2: 原状态 -> SDIO AF12 -> GPIO 输入态
```

### 2. 本轮不做

- 不调用 `HAL_SD_Init`、`HAL_SD_ConfigWideBusOperation`、`HAL_SD_ReadBlocks` 或 `HAL_SD_WriteBlocks`。
- 不初始化 SDIO 外设，不开启 SDIO 外设时钟，不配置 SDIO 寄存器。
- 不启用 SDIO 中断，不配置 `SDIO_IRQn`。
- 不接入 FATFS，不读写 SD 卡。
- 不调用 `HAL_DCMI_Start_DMA`、`HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不重新初始化 OV5640，不重新初始化 DCMI 外设。
- 不修改 UART DMA、二进制请求协议、OV56RGB5 帧格式、IWDG、FreeRTOS 任务优先级或任务栈大小。

### 3. 完整 SDIO GPIO 配置

本轮处理以下六个引脚：

| 引脚 | SDIO 功能 | 与 DCMI 的关系 |
| --- | --- | --- |
| PC8 | SDIO_D0 | 同时为 DCMI_D2，冲突 |
| PC9 | SDIO_D1 | 同时为 DCMI_D3，冲突 |
| PC10 | SDIO_D2 | 非 DCMI 冲突脚 |
| PC11 | SDIO_D3 | 同时为 DCMI_D4，冲突 |
| PC12 | SDIO_CK | 非 DCMI 冲突脚 |
| PD2 | SDIO_CMD | 非 DCMI 冲突脚 |

进入 AF12 时六个引脚统一使用：

```text
GPIO_MODE_AF_PP
GPIO_PULLUP
GPIO_SPEED_FREQ_VERY_HIGH
GPIO_AF12_SDIO
```

退出 AF12 时六个引脚统一先使用：

```text
GPIO_MODE_INPUT
GPIO_NOPULL
GPIO_SPEED_FREQ_LOW
```

随后仅将 PC8、PC9、PC11 配置回 `GPIO_AF13_DCMI`，PC10、PC12、PD2 不再二次配置，因此保持 GPIO 输入态。

### 4. 新增状态和错误码

新增状态字段：

- `sdio_full_gpio_switch_attempt_count`
- `sdio_full_gpio_switch_success_count`
- `sdio_full_gpio_switch_error_count`
- `sdio_full_gpio_restore_attempt_count`
- `sdio_full_gpio_restore_success_count`
- `sdio_full_gpio_restore_error_count`
- `sdio_full_gpio_af12_selected`
- `last_sdio_full_gpio_error_code`
- `last_sdio_full_gpio_operation_ms`

`last_sdio_full_gpio_operation_ms` 使用操作出口 tick 减入口 tick，记录本次完整 GPIO 切换或退出耗时，不记录系统绝对 tick。

新增错误码：

- `SDIO_FULL_GPIO_SWITCH_FAILED`
- `SDIO_FULL_GPIO_RESTORE_FAILED`

`SD STATUS` 和 `SD TAKEOVER STATUS` 同时输出以上状态字段，并增加派生文本字段 `last_sdio_full_gpio_error_text`。原有 PC8、PC9、PC11 冲突引脚和 AF12 状态字段、计数及字段名保持不变。

### 5. 预期命令行为

- 未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 仍返回 blocked；不释放 PC8、PC9、PC11，不切换任何 SDIO GPIO，并保持 `sdio_full_gpio_af12_selected=0`。
- `SNAPSHOT PREPARE` 后，ENTER 依次完成冲突引脚释放、PC8/PC9/PC11 AF12 切换和六个完整 SDIO GPIO AF12 配置。
- 完整配置成功后输出 `SD TAKEOVER ENTER: full SDIO GPIO switched to AF12, SD init is not implemented yet.`，状态进入 `ENTER_DEFERRED`，`sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=1`。
- ENTER 仍返回 `TAKEOVER_NOT_IMPLEMENTED`，表示 SDIO 外设、SD 卡和 FATFS 尚未初始化，不表示 GPIO 配置失败。
- AF12 状态下 `SD INIT` 仍保持 `NEED_TAKEOVER`，不会继续调用 `HAL_SD_Init`。
- `SD TAKEOVER EXIT` 优先将完整六引脚退回 GPIO 输入态；若只存在旧的三冲突引脚 AF12 状态，则保留 B11 的兼容退出路径。
- 完整退出和 DCMI AF13 恢复成功后输出 `SD TAKEOVER EXIT: full SDIO GPIO restored, conflict pins restored to DCMI AF13.`。
- EXIT 成功后 `sdio_af12_selected=0`、`sdio_full_gpio_af12_selected=0`、`conflict_pins_released=0`，PC10、PC12、PD2 保持输入态。
- `SNAPSHOT RESTORE` 后，basic、pc_dump、repeat 应恢复正常。

### 6. 回读和失败保护

- 完整 AF12 切换后，对 GPIOC 的 PC8～PC12 和 GPIOD 的 PD2 进行模式、上拉、速度、输出类型及 Alternate Function 最小寄存器回读。
- 完整退出后，检查六个引脚的 MODER 和 PUPDR 均符合无上下拉输入态。
- 完整 AF12 切换失败时进入 ERROR，返回 `SDIO_FULL_GPIO_SWITCH_FAILED`，不复位且不继续初始化 SDIO。
- 完整 GPIO 退出失败时进入 ERROR，返回 `SDIO_FULL_GPIO_RESTORE_FAILED`，不复位且不直接恢复 DCMI。
- PC8、PC9、PC11 恢复 DCMI AF13 失败时保留既有 `CONFLICT_PIN_RESTORE_FAILED` 错误路径。

### 7. 风险说明

- 本轮开始真实配置完整 SDIO GPIO 复用，但没有初始化 SDIO 外设，因此不会真正访问 SD 卡。
- PC12 为 SDIO_CK、PD2 为 SDIO_CMD；本轮只验证 GPIO 复用层面的闭环，不验证时钟、命令、数据或卡响应。
- OV5640 传感器本身可能仍在输出 DVP 数据，本轮仍未处理传感器 PWDN 或寄存器停流。
- 后续真实 SDIO 初始化前，需要继续确认 GPIOC/GPIOD 时钟状态、卡检测方式、初始化阶段 SDIO_CK 不超过 400 kHz、宽总线切换时序和所有错误恢复路径。

### 8. 后续板测计划

1. 确认启动正常，无 FATAL、反复复位或 IWDG 复位循环。
2. 未执行 `SNAPSHOT PREPARE` 时运行 `SD TAKEOVER ENTER`，确认被阻止且完整 GPIO 切换计数不增加。
3. 执行 `SNAPSHOT PREPARE`，确认 DCMI stop 成功且 snapshot guard 生效。
4. 执行 `SD TAKEOVER ENTER`，确认完整 SDIO GPIO 切换成功，`sdio_full_gpio_af12_selected=1`。
5. 执行 `SD INIT`，确认仍返回 `NEED_TAKEOVER`，`sdio_ready=0`、`fatfs_ready=0`。
6. 确认 guard 状态下文本 DUMP 和 binary 请求均被阻止。
7. 执行 `SD TAKEOVER EXIT`，确认六个引脚退出 AF12，PC8、PC9、PC11 恢复 DCMI AF13，`sdio_full_gpio_af12_selected=0`。
8. 执行 `SNAPSHOT RESTORE`，确认 guard 清除。
9. 执行 basic、pc_dump 和 repeat 20，确认图像恢复、CRC 正确且 `frame_id` 连续。
10. 检查 STATUS 中 IWDG、Hook、UART RX DMA、StreamBuffer、心跳、堆栈和协议统计均正常。

本轮 Codex 不执行硬件测试；烧录、串口命令和图像回归由用户在开发板上完成。

### 9. 后续 Stage 11C-2 建议

- 如果本轮单次完整 GPIO 切换验证通过，下一步先做多轮完整 SDIO GPIO AF12 切换循环稳定性验证。
- Stage 11C-2 仍不调用 `HAL_SD_Init`，仍不接入 FATFS，不读写 SD 卡。
- 重点验证多次完整六引脚 AF12 切换和退出后，PC8、PC9、PC11 的 DCMI AF13 恢复、RESTORE 图像链路和运行保护是否持续稳定。
- 多轮 GPIO 闭环通过后，再进入 `HAL_SD_Init` 最小初始化验证。

### 10. Stage 11C-1 板测结果

#### 10.1 启动情况

- 启动正常。
- `reset: iwdg=0`。
- `OV5640 ID = 0x5640`。
- `Camera init OK`。
- 无 FATAL。
- 无反复复位。
- 无 IWDG 复位循环。

#### 10.2 未执行 SNAPSHOT PREPARE 时的前置条件保护

直接执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

状态结果：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=0
takeover_precheck_fail_count=1
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pins_released=0
sdio_af12_selected=0
sdio_full_gpio_af12_selected=0
sdio_full_gpio_switch_attempt_count=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
```

结果符合预期：未执行 `SNAPSHOT PREPARE` 时，ENTER 被正确阻止；PC8、PC9、PC11 没有被释放或切换到 SDIO AF12，PC10、PC12、PD2 也没有被配置，完整 SDIO GPIO AF12 状态未进入。

#### 10.3 SNAPSHOT PREPARE

执行 `SNAPSHOT PREPARE`，输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

状态结果：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

`HAL_DCMI_Stop` 返回 HAL_OK，相机控制状态进入 `CAMERA_PAUSED`，软件 guard 和 DUMP 阻止条件均已生效。

#### 10.4 PREPARE 后完整 SDIO GPIO AF12 切换

执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: full SDIO GPIO switched to AF12, SD init is not implemented yet.
```

状态结果：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=2
takeover_precheck_attempt_count=2
takeover_precheck_success_count=1
takeover_precheck_fail_count=1
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=1
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_af12_selected=1
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
last_sdio_full_gpio_error_code=0
last_sdio_full_gpio_error_text=OK
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
```

PC8、PC9、PC11 已先释放为 GPIO 输入态，随后切换为 `GPIO_AF12_SDIO`；之后 PC8、PC9、PC10、PC11、PC12、PD2 完整切换为 `GPIO_AF12_SDIO`。`sdio_full_gpio_af12_selected=1` 表明六个引脚均已进入 SDIO AF12 复用状态。

`conflict_pins_released=0` 是合理结果，因为三个冲突引脚已经不是 GPIO 输入释放态。返回 `TAKEOVER_NOT_IMPLEMENTED` 也是预期行为，因为本阶段仍未调用 `HAL_SD_Init`，也未接入 FATFS。

#### 10.5 AF12 状态下的 SD INIT 边界验证

执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

SD 状态：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
takeover_state_text=ENTER_DEFERRED
sdio_af12_selected=1
sdio_full_gpio_af12_selected=1
```

即使 PC8、PC9、PC10、PC11、PC12、PD2 已完整切换到 SDIO AF12，`SD INIT` 仍保持 `NEED_TAKEOVER`。本轮没有初始化 SDIO，没有调用 `HAL_SD_Init`，没有接入 FATFS，也没有读写 SD 卡。

本次 `last_operation_ms=41131` 仍更像系统绝对 tick，属于既有 SD INIT 耗时统计问题；该问题不影响 Stage 11C-1 的完整 GPIO 复用验证结论，后续单独修正。

#### 10.6 guard 状态下 DUMP 和 binary 阻止验证

文本 DUMP 输出：

```text
DUMP blocked: snapshot software guard active.
```

- 未发送 OV56RGB5 图像帧。
- `dump_block_count=1`。
- guard 状态下执行 binary basic，响应长度为 0 B，PC 端接收超时，测试结果为 FAIL。
- 此处 FAIL 是软件 guard 生效后的预期现象。
- `binary_block_count=1`。

#### 10.7 SD TAKEOVER EXIT 完整恢复

执行 `SD TAKEOVER EXIT`，输出：

```text
SD TAKEOVER EXIT: full SDIO GPIO restored, conflict pins restored to DCMI AF13.
```

状态结果：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
sdio_af12_selected=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
last_sdio_full_gpio_error_code=0
last_sdio_full_gpio_error_text=OK
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
```

PC8、PC9、PC10、PC11、PC12、PD2 已从完整 SDIO AF12 状态退出为 GPIO 输入态；随后 PC8、PC9、PC11 恢复为 DCMI AF13，PC10、PC12、PD2 保持 GPIO 输入态。

`sdio_af12_restore_attempt_count=0` 是合理结果：本轮进入的是完整六引脚状态，EXIT 直接调用完整 SDIO GPIO 退出函数，不再调用旧的三冲突引脚 AF12 退出函数。本轮仍未调用 `HAL_DCMI_Start_DMA`，也未重新初始化 OV5640 或 DCMI 外设。

#### 10.8 SNAPSHOT RESTORE 与前置条件复位

执行 `SNAPSHOT RESTORE`，输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

状态结果：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
restore_attempt_count=1
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

RESTORE 后再次执行 `SD TAKEOVER ENTER`，输出：

```text
SD TAKEOVER ENTER: blocked, run SNAPSHOT PREPARE first.
```

状态结果：

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=3
takeover_precheck_attempt_count=3
takeover_precheck_success_count=1
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pins_released=0
sdio_af12_selected=0
sdio_full_gpio_af12_selected=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
```

RESTORE 后接管前置条件重新失效，说明完整 SDIO GPIO AF12 切换退出后，软件安全门状态已恢复正常。

#### 10.9 RESTORE 后图像功能回归

- 第一次 guard 状态下 binary basic 响应长度为 0 B、结果 FAIL，属于预期阻止现象。
- RESTORE 后 basic：PASS，`frame_id=1`。
- RESTORE 后 pc_dump：PASS，`frame_id=2`，图像质量无警告。
- RESTORE 后 repeat：20/20 PASS。
- repeat 的 `frame_id` 从 3 到 22 连续递增。
- repeat 平均耗时 3465.68 ms。
- repeat 最短耗时 3446.21 ms。
- repeat 最长耗时 3469.50 ms。

#### 10.10 最终 SD TAKEOVER STATUS

```text
takeover_state=0
takeover_state_text=IDLE
takeover_enter_attempt_count=3
takeover_exit_attempt_count=1
takeover_error_count=0
last_takeover_error_code=9
last_takeover_error_text=SNAPSHOT_NOT_PAUSED
takeover_precheck_attempt_count=3
takeover_precheck_success_count=1
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_restore_attempt_count=0
sdio_af12_restore_success_count=0
sdio_af12_restore_error_count=0
sdio_af12_selected=0
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
last_sdio_full_gpio_error_code=0
last_sdio_full_gpio_error_text=OK
```

最终状态表明完整六引脚切换与退出各成功一次、错误计数均为 0，三个冲突引脚也成功恢复为 DCMI AF13。最终 `takeover_state=IDLE` 和 `SNAPSHOT_NOT_PAUSED` 来自 RESTORE 后再次执行 ENTER 的预期前置条件阻止。

#### 10.11 最终 SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

`restore_success_count=0` 和 `CAMERA_RESTORE_NOT_IMPLEMENTED` 符合当前阶段边界：RESTORE 清除软件 guard，但尚未实现 DCMI 硬件重启；现有图像请求链路在后续回归中仍可恢复工作。

#### 10.12 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

统计解释：

- `dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 binary 请求各被阻止一次。
- `binary_request_error_count=0` 是正确结果，因为二进制请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。
- `last_error_code=8` 对应 snapshot guard active 类错误，属于本轮 guard 阻止路径的预期记录。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7520
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=1
monitor_heartbeat_age_ms=836
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=165
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=53
uart_dma_rx_bytes=643
stream_buffer_write_bytes=643
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

#### 10.13 板测结论

Stage 11C-1 验证通过。未执行 `SNAPSHOT PREPARE` 时，`SD TAKEOVER ENTER` 被正确阻止，且未释放冲突引脚、未切换任何 SDIO AF12。`SNAPSHOT PREPARE` 成功后，ENTER 能将 PC8、PC9、PC11 先释放为 GPIO 输入态，再将 PC8、PC9、PC10、PC11、PC12、PD2 完整切换为 `GPIO_AF12_SDIO`，并记录 `sdio_full_gpio_af12_selected=1`。

AF12 状态下 `SD INIT` 仍保持 `NEED_TAKEOVER`，没有初始化 SDIO 或 FATFS。`SD TAKEOVER EXIT` 能将六个引脚从 SDIO AF12 退出为 GPIO 输入态，并恢复 PC8、PC9、PC11 为 DCMI AF13，最终记录 `sdio_full_gpio_af12_selected=0`。随后 `SNAPSHOT RESTORE` 清除 guard，RESTORE 后 basic、pc_dump、repeat 均恢复正常。

最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明单次完整 SDIO GPIO AF12 切换闭环没有破坏现有摄像头采集、DUMP、二进制请求和运行保护机制。

#### 10.14 适用边界与严谨说明

本轮虽然真实配置了 PC8、PC9、PC10、PC11、PC12、PD2 的 `GPIO_AF12_SDIO`，但没有初始化 SDIO 外设，没有调用 `HAL_SD_Init`，没有接入 FATFS，也没有访问 SD 卡。因此，本轮通过只能说明 MCU 侧完整 SDIO GPIO AF12 切换闭环可用，不能说明 SD 卡通信已经可用。

#### 10.15 后续 Stage 11C-2 建议

Stage 11C-2 建议新增 PC 端自动化脚本，执行多轮以下完整流程：

```text
SNAPSHOT PREPARE
SD TAKEOVER ENTER
SD INIT 保持 NEED_TAKEOVER
DUMP_BLOCK
BINARY_BLOCK
SD TAKEOVER EXIT
SNAPSHOT RESTORE
RESTORE_BINARY
```

Stage 11C-2 仍不调用 `HAL_SD_Init`，不接入 FATFS；先验证完整六引脚 SDIO AF12 多轮切换闭环稳定性，再进入真实 SDIO 最小初始化。

## Stage 11C-2 多轮完整SDIO GPIO AF12切换闭环稳定性验证

### 1. 本轮目的

- 新增独立 PC 端测试工具 `tools/uart_snapshot_full_sdio_gpio_cycle_test.py`。
- 自动执行多轮 `SNAPSHOT PREPARE / SD TAKEOVER ENTER / SD INIT / DUMP_BLOCK / BINARY_BLOCK / SD TAKEOVER EXIT / SNAPSHOT RESTORE / RESTORE_BINARY` 完整流程。
- 验证 PC8、PC9、PC10、PC11、PC12、PD2 多次完整切换到 SDIO AF12、再退出 AF12 后图像链路是否持续稳定。
- 验证每轮 EXIT 后 PC8、PC9、PC11 恢复为 DCMI AF13，PC10、PC12、PD2 保持 GPIO 输入态。
- 验证完整 AF12 状态下 `SD INIT` 始终保持 `NEED_TAKEOVER`，不会真实初始化 SDIO 或 FATFS。
- 验证 snapshot guard 生效期间，文本 DUMP 和 binary 图像请求持续被阻止。
- 验证 `SNAPSHOT RESTORE` 后 binary 图像请求持续恢复，并检查 `frame_id` 连续递增。

### 2. 本轮不做

- 不修改任何固件 C/H 源码，不修改 Core 或 BSPDrivers。
- 不新增 `HAL_DCMI_Stop`，不调用 `HAL_DCMI_Start_DMA`、`HAL_DMA_Abort` 或 `HAL_DMA_DeInit`。
- 不修改完整 SDIO GPIO AF12 切换固件实现，不新增 `GPIO_AF12_SDIO` 固件代码。
- 不初始化 SDIO，不调用任何 HAL SD API。
- 不配置 `SDIO_IRQn`，不启用 SDIO 中断。
- 不接入 FATFS，不读写 SD 卡。
- 不修改 UART/二进制图像请求协议，也不修改任何现有 Python 工具。

### 3. 测试脚本

脚本路径：

```text
tools/uart_snapshot_full_sdio_gpio_cycle_test.py
```

默认参数：

```text
port=COM4
baud=115200
cycles=5
guard_timeout=2.0 s
frame_timeout=10.0 s
interval=0.2 s
tag=stage11_c2_full_sdio_gpio_cycle
DTR=False
RTS=False
```

脚本只依赖 pyserial 和 Python 标准库，不导入现有项目脚本，不使用类、线程或 async，不保存 PNG，也不做图像质量分析。

每轮按以下顺序执行：

1. `SNAPSHOT PREPARE` 必须包含 `DCMI stop OK`。
2. `SD TAKEOVER ENTER` 必须包含 `full SDIO GPIO switched to AF12`、`sdio_full_gpio_af12_selected=1` 和完整 GPIO 切换成功计数字段。
3. `SD INIT` 必须包含 `NEED_TAKEOVER`、`is_initialized=0`、`sdio_ready=0`、`fatfs_ready=0` 和 `sdio_full_gpio_af12_selected=1`。
4. 文本 `DUMP` 必须被 snapshot guard 阻止。
5. guard 状态下发送 binary 图像请求，不得收到合法 OV56RGB5 图像帧；0 B、超时或无合法 magic 均属于预期。
6. `SD TAKEOVER EXIT` 必须包含 `full SDIO GPIO restored, conflict pins restored to DCMI AF13`、`sdio_full_gpio_af12_selected=0`、完整 GPIO 退出成功计数和冲突引脚恢复成功计数。
7. `SNAPSHOT RESTORE` 响应必须包含 `SNAPSHOT RESTORE` 或 `RESTORE_DEFERRED`。
8. RESTORE 后 binary 请求必须收到合法 OV56RGB5 图像帧，并通过 version、pixel format、160x120 尺寸、38400 B payload 和 CRC32 校验。

每轮输出格式为：

```text
[01/05] PREPARE=PASS FULL_SDIO_ENTER=PASS SD_INIT_DEFERRED=PASS DUMP_BLOCK=PASS BINARY_BLOCK=PASS FULL_SDIO_EXIT=PASS RESTORE=PASS RESTORE_BINARY=PASS frame_id=xx time=xxxx ms
```

单项失败时，脚本打印失败原因、写入本轮 CSV，默认继续后续步骤和下一轮；只有串口打开失败时直接结束。

### 4. 协议和结果校验

脚本自行构造 14 字节二进制图像请求：magic 为 `A5 5A`，version 为 1，type 为 `0x20`，seq 为小端 uint16，len 固定为 0，对 version/type/seq/len 六字节计算 CRC32，结尾为 `0D 0A`。

RESTORE 后响应必须是总长 38426 B 的 OV56RGB5 图像帧：22 B header、38400 B RGB565 payload 和 4 B CRC。脚本检查 magic、version、pixel format、width、height、payload_len 和 payload CRC，并使用 `frame_id` 判断跨轮图像是否逐帧加一。

### 5. 测试统计与输出文件

脚本统计：

- `cycle_total`
- `prepare_ok_count`
- `full_sdio_enter_ok_count`
- `sd_init_deferred_ok_count`
- `text_dump_block_ok_count`
- `binary_block_ok_count`
- `full_sdio_exit_ok_count`
- `restore_command_ok_count`
- `restore_binary_ok_count`
- `fail_count`
- `first_frame_id`
- `last_frame_id`
- `frame_id_continuous`
- `avg_restore_binary_time_ms`
- `min_restore_binary_time_ms`
- `max_restore_binary_time_ms`

脚本自动创建 `captures` 目录并生成：

```text
captures/full_sdio_gpio_cycle_<tag>_<timestamp>.csv
captures/full_sdio_gpio_cycle_<tag>_<timestamp>_summary.txt
```

CSV 保存每轮八项结果、`frame_id`、RESTORE 后图像请求耗时和错误原因。summary 保存测试参数、全部统计、`frame_id` 连续性、最终 PASS/FAIL，并明确说明 guard binary 超时是预期现象、ENTER 后六个 GPIO 必须进入 AF12、`SD INIT` 必须保持 `NEED_TAKEOVER`、EXIT 后完整 GPIO 必须退出 AF12且冲突引脚恢复 DCMI AF13、RESTORE 后 binary 必须 PASS。

总测试只有在每轮八项检查全部通过、`fail_count=0`、RESTORE 后成功帧数量等于循环次数且 `frame_id` 连续递增时才判定为 PASS。

### 6. 后续板测计划

1. 先执行 5 轮：

   ```text
   python tools/uart_snapshot_full_sdio_gpio_cycle_test.py --cycles 5 --tag stage11_c2_5cycle
   ```

2. 5 轮通过后再执行 20 轮：

   ```text
   python tools/uart_snapshot_full_sdio_gpio_cycle_test.py --cycles 20 --tag stage11_c2_20cycle
   ```

3. 最后通过串口依次执行：

   ```text
   SD TAKEOVER STATUS
   SNAPSHOT STATUS
   STATUS
   ```

4. 确认最终 `sdio_full_gpio_af12_selected=0`，完整 GPIO 切换/退出和冲突引脚恢复计数符合循环次数，错误计数均为 0。
5. 检查 IWDG、Hook、UART RX DMA、StreamBuffer、任务心跳、堆栈和图像请求统计。
6. 若 20 轮全部通过且运行保护状态正常，再进入下一阶段。

本轮 Codex 只进行脚本静态编译检查，不打开 COM4，也不执行硬件测试；循环结果由用户在开发板上验证并回填。

### 7. 后续 Stage 11C-3 建议

- 如果 Stage 11C-2 多轮验证稳定，可进入 `HAL_SD_Init` 最小初始化阶段。
- Stage 11C-3 先只实现 SDIO 初始化骨架和初始化状态观测，仍不接入 FATFS、不写 SD 卡。
- `SD INIT` 只能在完整 SDIO GPIO AF12 状态下调用 `HAL_SD_Init`，继续保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序。
- 初始化阶段必须保证 SDIO_CK 不超过 400 kHz，初始化成功后才考虑提高时钟或配置宽总线。
- `HAL_SD_Init` 失败时必须记录明确错误码，并保留退出完整 AF12、恢复 DCMI AF13 和清除 guard 的可恢复路径；不允许通过复位处理失败。

### 8. Stage 11C-2 板测结果

#### 8.1 5 轮循环测试

执行命令：

```text
python tools/uart_snapshot_full_sdio_gpio_cycle_test.py --cycles 5 --tag stage11_c2_5cycle
```

测试结果：

```text
cycle_total=5
prepare_ok_count=5
full_sdio_enter_ok_count=5
sd_init_deferred_ok_count=5
text_dump_block_ok_count=5
binary_block_ok_count=5
full_sdio_exit_ok_count=5
restore_command_ok_count=5
restore_binary_ok_count=5
fail_count=0
first_frame_id=23
last_frame_id=27
frame_id_continuous=是
avg_restore_binary_time_ms=3442.07
min_restore_binary_time_ms=3431.81
max_restore_binary_time_ms=3462.28
测试结果=PASS
```

输出文件：

```text
captures\full_sdio_gpio_cycle_stage11_c2_5cycle_20260805_173210.csv
captures\full_sdio_gpio_cycle_stage11_c2_5cycle_20260805_173210_summary.txt
```

#### 8.2 20 轮循环测试

执行命令：

```text
python tools/uart_snapshot_full_sdio_gpio_cycle_test.py --cycles 20 --tag stage11_c2_20cycle
```

测试结果：

```text
cycle_total=20
prepare_ok_count=20
full_sdio_enter_ok_count=20
sd_init_deferred_ok_count=20
text_dump_block_ok_count=20
binary_block_ok_count=20
full_sdio_exit_ok_count=20
restore_command_ok_count=20
restore_binary_ok_count=20
fail_count=0
first_frame_id=28
last_frame_id=47
frame_id_continuous=是
avg_restore_binary_time_ms=3458.75
min_restore_binary_time_ms=3425.28
max_restore_binary_time_ms=3490.02
测试结果=PASS
```

输出文件：

```text
captures\full_sdio_gpio_cycle_stage11_c2_20cycle_20260805_173312.csv
captures\full_sdio_gpio_cycle_stage11_c2_20cycle_20260805_173312_summary.txt
```

#### 8.3 C-2 脚本合计 25 轮结果

5 轮和 20 轮测试均通过，C-2 脚本合计完成 25 轮以下完整流程：

```text
SNAPSHOT PREPARE
SD TAKEOVER ENTER
SD INIT
guard 文本 DUMP 阻止
guard binary 阻止
SD TAKEOVER EXIT
SNAPSHOT RESTORE
RESTORE 后 binary PASS
```

合计结果：

- `SNAPSHOT PREPARE` 成功 25 次。
- `SD TAKEOVER ENTER` 成功检测到 `full SDIO GPIO switched to AF12` 25 次。
- `SD INIT` 保持 `NEED_TAKEOVER` 25 次。
- guard 状态下文本 DUMP 被阻止 25 次。
- guard 状态下 binary 请求被阻止 25 次。
- `SD TAKEOVER EXIT` 成功检测到 `full SDIO GPIO restored, conflict pins restored to DCMI AF13` 25 次。
- `SNAPSHOT RESTORE` 响应正常 25 次。
- RESTORE 后 binary 请求成功 25 次。
- RESTORE 后 `frame_id` 从 23 到 47 连续递增。
- 测试期间未观察到复位、FATAL 或 COM4 占用。

#### 8.4 最终累计计数来源说明

最终 SD TAKEOVER STATUS 中部分成功计数为 26，而不是 25。原因是执行 C-2 自动化测试前开发板没有复位，Stage 11C-1 手动板测中的 1 次完整 SDIO GPIO AF12 切换和恢复仍保留在固件状态计数中。

因此应区分：

- C-2 脚本本身新增并验证 25 轮，25/25 全部 PASS。
- 固件最终状态包含 C-1 手动测试 1 次和 C-2 自动测试 25 次，累计为 26 次成功切换和恢复。
- C-2 是否通过应同时依据脚本 25 轮结果和最终累计状态中错误计数均为 0，而不能简单要求最终计数等于 25。

#### 8.5 最终 SD TAKEOVER STATUS

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=28
takeover_exit_attempt_count=26
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
takeover_precheck_attempt_count=28
takeover_precheck_success_count=26
takeover_precheck_fail_count=2
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=26
conflict_pin_release_success_count=26
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=26
conflict_pin_restore_success_count=26
conflict_pin_restore_error_count=0
conflict_pins_released=0
last_conflict_pin_error_code=0
last_conflict_pin_error_text=OK
sdio_af12_switch_attempt_count=26
sdio_af12_switch_success_count=26
sdio_af12_switch_error_count=0
sdio_af12_restore_attempt_count=0
sdio_af12_restore_success_count=0
sdio_af12_restore_error_count=0
sdio_af12_selected=0
last_sdio_af12_error_code=0
last_sdio_af12_error_text=OK
sdio_full_gpio_switch_attempt_count=26
sdio_full_gpio_switch_success_count=26
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_restore_attempt_count=26
sdio_full_gpio_restore_success_count=26
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
last_sdio_full_gpio_error_code=0
last_sdio_full_gpio_error_text=OK
```

状态解释：

- `sdio_full_gpio_switch_success_count=26` 表明完整 SDIO GPIO 切换到 AF12 累计成功 26 次。
- `sdio_full_gpio_restore_success_count=26` 表明完整 SDIO GPIO 从 AF12 退出累计成功 26 次。
- `conflict_pin_restore_success_count=26` 表明 PC8、PC9、PC11 累计恢复为 DCMI AF13 成功 26 次。
- `sdio_full_gpio_af12_selected=0`、`sdio_af12_selected=0`、`conflict_pins_released=0` 表明最终已退出 SDIO AF12，并恢复到安全状态。
- `sdio_af12_restore_attempt_count=0` 是合理结果：当前走完整六引脚退出路径，不调用旧的三冲突引脚 AF12 退出函数。
- `last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED` 是预期现象，因为本阶段仍未实现真实 SDIO 初始化。

#### 8.6 最终 SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=26
restore_attempt_count=26
prepare_success_count=26
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=26
dcmi_stop_success_count=26
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=0
dump_block_required=0
dump_block_count=26
binary_block_count=26
```

`dcmi_stop_success_count=26` 表明 `HAL_DCMI_Stop` 累计调用稳定。最终 `software_guard_active=0`、`dump_block_required=0` 表明 RESTORE 后 guard 已清除。`dump_block_count=26` 和 `binary_block_count=26` 是预期结果，包含 C-1 手动测试 1 次和 C-2 自动化测试 25 次。

#### 8.7 最终 STATUS 关键字段

`RTOS`：

```text
dump_request_count=99
dump_success_count=47
dump_error_count=52
binary_request_count=72
binary_request_success_count=46
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=40
last_binary_error_code=0
last_error_code=8
```

统计解释：

- 最终 RTOS 计数包含 C-1 手动测试和 C-2 自动化测试，不是单独 C-2 的 25 轮干净计数。
- C-2 脚本自身新增 25 次 RESTORE 后 binary 成功、25 次 guard binary 阻止和 25 次 guard 文本 DUMP 阻止。
- `binary_request_error_count=0` 是正确结果，因为 guard 状态下二进制请求格式正确，只是被 snapshot guard 拦截，不属于协议错误。
- `last_error_code=8` 对应 snapshot guard active 类错误，属于预期记录。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=7520
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=0
monitor_heartbeat_age_ms=666
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=6309
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=302
uart_dma_rx_bytes=3588
stream_buffer_write_bytes=3588
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

#### 8.8 板测结论

Stage 11C-2 验证通过。新增 `uart_snapshot_full_sdio_gpio_cycle_test.py` 后，5 轮和 20 轮多轮完整 SDIO GPIO AF12 切换闭环测试均 PASS。C-2 脚本合计完成 25 轮，RESTORE 后 `frame_id` 从 23 到 47 连续递增。

最终累计状态显示完整 SDIO GPIO 切换到 AF12 成功 26 次、从 AF12 退出成功 26 次、PC8、PC9、PC11 恢复为 DCMI AF13 成功 26 次，错误均为 0，最终 `sdio_full_gpio_af12_selected=0`、`sdio_af12_selected=0`、`conflict_pins_released=0`。最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步，说明完整 SDIO GPIO AF12 多轮切换闭环稳定。

#### 8.9 适用边界与严谨说明

本轮只验证了 PC8、PC9、PC10、PC11、PC12、PD2 在 GPIO 输入态与 `GPIO_AF12_SDIO` 之间的多轮切换稳定性，以及 PC8、PC9、PC11 恢复 DCMI AF13 后图像链路可恢复。虽然 AF12 状态下 `SD INIT` 保持 `NEED_TAKEOVER`，且 RESTORE 后图像导出持续恢复正常，但本轮没有初始化 SDIO 外设，没有调用 `HAL_SD_Init`，没有接入 FATFS，也没有访问 SD 卡。因此，本轮通过不能说明真实 SD 卡通信已经可用。

#### 8.10 后续 Stage 11C-3 建议

Stage 11C-2 通过后，Stage 11C-3 可以进入 `HAL_SD_Init` 最小初始化验证，建议先实现：

- 扩展 SDIO 初始化状态字段。
- 建立 `HAL_SD_Init` 最小调用路径，只观察返回值和 `HAL_SD_GetError`。
- 保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序，仅在完整 SDIO GPIO AF12 状态下执行初始化。
- 不接入 FATFS，不写 SD 卡。
- 初始化失败时不复位，必须记录明确错误码，并允许执行 `SD TAKEOVER EXIT` 和 `SNAPSHOT RESTORE` 完成恢复。

## Stage 11C-3 HAL_SD_Init最小初始化验证

### 1. 本轮目的

- 保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序，仅在 PC8、PC9、PC10、PC11、PC12、PD2 已完整切换到 `GPIO_AF12_SDIO`，且 `sdio_full_gpio_af12_selected=1` 时执行 `HAL_SD_Init`。
- 只观察 `HAL_SD_Init` 返回值和 `HAL_SD_GetError`，验证真实 HAL SD 最小初始化调用路径是否可控、可观察、可恢复。
- 初始化成功时记录 `is_initialized=1`、`sdio_ready=1`、`fatfs_ready=0`。
- 初始化失败时记录 HAL 返回状态和错误码，但不复位、不进入 FATAL、不卡死，并保留退出恢复路径。
- `SD TAKEOVER EXIT` 在 SDIO 已初始化或时钟已打开时先执行 `HAL_SD_DeInit`，随后关闭 SDIO 外设时钟，再退出完整 AF12 并恢复 PC8、PC9、PC11 的 DCMI AF13。
- 后续板测需要验证 `SNAPSHOT RESTORE` 后 basic、pc_dump、repeat 和运行保护状态均可恢复正常。本轮 Codex 不执行硬件测试。

### 2. 本轮不做

- 不调用 `HAL_SD_ConfigWideBusOperation`，只使用 1-bit 总线模式。
- 不调用 `HAL_SD_ReadBlocks` 或 `HAL_SD_WriteBlocks`，不读写任何 SD 卡块。
- 不接入 FATFS，不调用 `f_mount`、`f_open`、`f_write`、`f_read`，不创建或写入文件。
- 不使用 SDIO DMA，不启用 SDIO 中断，也不配置 `SDIO_IRQn`。
- 不修改 DCMI、FreeRTOS、IWDG、UART DMA、二进制请求协议或 OV56RGB5 图像帧格式。

### 3. HAL SD最小配置

模块内部使用静态 `SD_HandleTypeDef hsd_snapshot`，配置如下：

```text
Instance=SDIO
ClockEdge=SDIO_CLOCK_EDGE_RISING
ClockBypass=SDIO_CLOCK_BYPASS_DISABLE
ClockPowerSave=SDIO_CLOCK_POWER_SAVE_DISABLE
BusWide=SDIO_BUS_WIDE_1B
HardwareFlowControl=SDIO_HARDWARE_FLOW_CONTROL_DISABLE
ClockDiv=118U
```

`ClockDiv=118U` 用于保守的 SDIO 初始化低速阶段。本轮不切换 4-bit，不做 FATFS 和块读写。`HAL_SD_Init` 前打开 SDIO 外设时钟；EXIT 中无论初始化成功还是失败，只要时钟打开过，都尝试 `HAL_SD_DeInit`、关闭时钟并继续恢复 GPIO。

### 4. 新增状态字段

- `real_hal_sd_init_enabled`
- `sdio_clock_enabled`
- `sdio_hal_init_attempt_count`
- `sdio_hal_init_success_count`
- `sdio_hal_init_error_count`
- `sdio_hal_deinit_attempt_count`
- `sdio_hal_deinit_success_count`
- `sdio_hal_deinit_error_count`
- `last_hal_sd_init_status`
- `last_hal_sd_deinit_status`
- `last_hal_sd_error`
- `last_sdio_hal_init_operation_ms`
- `last_sdio_hal_deinit_operation_ms`

其中 `real_hal_sd_init_enabled` 固定为 1；被 `NEED_TAKEOVER` 阻止的 `SD INIT` 不增加真实 HAL 初始化调用次数；两个 HAL 操作耗时均使用出口 tick 减入口 tick，不记录系统绝对 tick。

### 5. 预期命令行为

- 未完成 `SNAPSHOT PREPARE / SD TAKEOVER ENTER` 时，`SD INIT` 不调用 HAL，仍返回 `NEED_TAKEOVER`。
- 完整 SDIO GPIO AF12 已选择后，`SD INIT` 打开 SDIO 时钟并调用 `HAL_SD_Init`。
- SD 卡初始化正常时输出 `SD INIT: HAL_SD_Init OK, FATFS is not mounted.`。
- SD 卡未插入、不兼容或存在时钟、线序、硬件问题时，输出 `SD INIT: HAL_SD_Init failed, status=<status>, error=0x<error>.`，并记录 `SDIO_HAL_INIT_FAILED`。
- `SD TAKEOVER EXIT` 若执行了反初始化，可输出 `HAL_SD_DeInit` 状态和错误码；即使反初始化失败，也必须关闭 SDIO 时钟并继续恢复 GPIO。
- `SNAPSHOT RESTORE` 后应继续验证 basic、pc_dump、repeat 20/20 PASS，以及 IWDG、HOOK、UART RX DMA 状态正常。

### 6. 风险说明

- 本轮首次打开 SDIO 外设时钟并调用 `HAL_SD_Init`，风险高于此前只验证 GPIO AF12 切换的阶段。
- `HAL_SD_Init` 可能因 SD 卡未插入、卡不兼容、时钟配置、线序或硬件问题失败；失败不一定立即代表软件实现错误，但必须能够输出状态、执行 EXIT 并恢复相机链路。
- `HAL_SD_Init` 或 `HAL_SD_DeInit` 失败不得触发复位、FATAL 或死锁；GPIO 恢复路径不能被反初始化失败阻断。
- 本轮不读取或写入 SD 卡，不会主动改动用户卡内文件。

### 7. 后续Stage 11C-4建议

- 如果 `HAL_SD_Init` 成功，下一步使用 `HAL_SD_GetCardInfo` 读取 SD 卡基础信息。
- 继续不接 FATFS、不写卡，先验证卡类型、容量、块大小、错误码和退出恢复路径。

### 8. Stage 11C-3板测结果

#### 8.1 启动情况

- 启动正常，`reset: iwdg=0`。
- OV5640 ID 为 `0x5640`，Camera init OK。
- 无 FATAL、无反复复位、无 IWDG 复位循环。

#### 8.2 未PREPARE时的SD INIT保护

未执行 `SNAPSHOT PREPARE` 时直接执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

此时 `SD STATUS` 为：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
takeover_state_text=IDLE
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=0
sdio_hal_init_success_count=0
sdio_hal_init_error_count=0
last_hal_sd_init_status=0
last_hal_sd_error=0
```

该结果说明未执行 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 时，`SD INIT` 被 `NEED_TAKEOVER` 正确阻止；没有调用 `HAL_SD_Init`，没有打开 SDIO 时钟，也没有初始化 SDIO 或 FATFS。

#### 8.3 SNAPSHOT PREPARE

命令输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

`SNAPSHOT STATUS` 为：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

`HAL_DCMI_Stop` 调用成功，snapshot guard 已开启；此时图像 DUMP 和 binary 请求应被阻止。

#### 8.4 完整SDIO GPIO AF12接管

`SD TAKEOVER ENTER` 输出：

```text
SD TAKEOVER ENTER: full SDIO GPIO switched to AF12, run SD INIT next.
```

`SD TAKEOVER STATUS` 为：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=1
takeover_precheck_fail_count=0
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=1
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_af12_selected=1
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
```

PC8、PC9、PC11 已先释放并切换到 SDIO AF12，随后 PC8、PC9、PC10、PC11、PC12、PD2 已完整切换到 SDIO AF12。此时尚未调用 `HAL_SD_Init`，因此 `sdio_clock_enabled=0` 是正常结果。

#### 8.5 HAL_SD_Init最小初始化

在完整 SDIO GPIO AF12 状态下执行 `SD INIT`，输出：

```text
SD INIT: HAL_SD_Init OK, FATFS is not mounted.
```

`SD STATUS` 为：

```text
is_initialized=1
takeover_required=1
sdio_ready=1
fatfs_ready=0
init_attempt_count=2
init_success_count=1
init_error_count=0
last_error_code=0
last_error_text=OK
last_operation_ms=4
takeover_state_text=ENTER_DEFERRED
sdio_af12_selected=1
sdio_full_gpio_af12_selected=1
real_hal_sd_init_enabled=1
sdio_clock_enabled=1
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=0
sdio_hal_deinit_success_count=0
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
last_sdio_hal_init_operation_ms=4
last_sdio_hal_deinit_operation_ms=0
```

`HAL_SD_Init` 返回 `HAL_OK`，`HAL_SD_GetError` 返回 0，最小初始化耗时约 4 ms。初始化后 `is_initialized=1`、`sdio_ready=1`、`fatfs_ready=0`，符合本阶段只初始化 SDIO、不挂载 FATFS 的设计。

`init_attempt_count=2` 是正确结果：第一次 `SD INIT` 在未 PREPARE、未接管时被 `NEED_TAKEOVER` 阻止，第二次才真正调用 `HAL_SD_Init`；`sdio_hal_init_attempt_count=1` 才是实际 HAL 初始化调用次数。本轮没有调用 `HAL_SD_ConfigWideBusOperation`，没有读写 SD 卡块，没有接入 FATFS，也没有写文件。

#### 8.6 guard状态下的DUMP和binary保护

文本 `DUMP` 输出：

```text
DUMP blocked: snapshot software guard active.
```

此时 `SNAPSHOT STATUS` 中：

```text
software_guard_active=1
dump_block_required=1
dump_block_count=1
binary_block_count=0
```

guard 状态下执行 binary basic，响应长度为 0 B，PC 端接收超时，测试结果为 FAIL。该 FAIL 是预期现象；随后 `binary_block_count` 增加。`HAL_SD_Init` 成功后 snapshot guard 仍然有效，文本 DUMP 和 binary 图像请求均被阻止，没有输出 OV56RGB5 图像帧。

#### 8.7 SD TAKEOVER EXIT与HAL_SD_DeInit

命令输出：

```text
SD TAKEOVER EXIT: HAL_SD_DeInit status=0, error=0x00000000.
SD TAKEOVER EXIT: full SDIO GPIO restored, conflict pins restored to DCMI AF13.
```

退出后的 `SD TAKEOVER STATUS / SD STATUS` 关键字段为：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
sdio_af12_selected=0
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
last_sdio_hal_init_operation_ms=4
last_sdio_hal_deinit_operation_ms=0
is_initialized=0
sdio_ready=0
fatfs_ready=0
```

`HAL_SD_DeInit` 返回 `HAL_OK`，SDIO 时钟已关闭。完整 SDIO GPIO 已退出 AF12，PC8、PC9、PC11 已恢复为 DCMI AF13。EXIT 后 `is_initialized=0`、`sdio_ready=0` 是正确结果，因为已经完成反初始化。`sdio_af12_restore_attempt_count=0` 也是合理结果：当前走完整六引脚退出路径，不调用旧的三冲突引脚退出函数。

#### 8.8 SNAPSHOT RESTORE

命令输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

`SNAPSHOT STATUS` 为：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

`SNAPSHOT RESTORE` 清除了 snapshot guard，RESTORE 后允许恢复图像导出。

#### 8.9 RESTORE后图像功能回归

- guard 状态下 basic 响应长度为 0 B，测试 FAIL，属于预期现象。
- RESTORE 后 basic 响应长度为 38426 B，`frame_id=1`，CRC 一致，测试 PASS。
- RESTORE 后 pc_dump 测试 PASS，`frame_id=2`，图像质量无阈值警告。
- pc_dump 图像：`captures/014_sd_c3_hal_sd_init_20260805_193525.png`。
- pc_dump 报告：`captures/014_sd_c3_hal_sd_init_20260805_193525_report.txt`。
- RESTORE 后 repeat 共请求 20 次，成功 20 次、失败 0 次，成功率 100.00%，测试 PASS。
- repeat 平均耗时 3462.97 ms，最短 3438.53 ms，最长 3467.90 ms。
- repeat 的 `frame_id` 从 3 到 22 连续递增。

#### 8.10 最终SD STATUS

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=2
init_success_count=1
init_error_count=0
last_error_code=0
last_error_text=OK
last_operation_ms=4
takeover_state_text=EXIT_DEFERRED
sdio_af12_selected=0
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
last_sdio_hal_init_operation_ms=4
last_sdio_hal_deinit_operation_ms=0
```

#### 8.11 最终SD TAKEOVER STATUS

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=1
takeover_exit_attempt_count=1
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
takeover_precheck_attempt_count=1
takeover_precheck_success_count=1
takeover_precheck_fail_count=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=0
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
```

`last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED` 仍是预期结果，因为当前只完成 HAL SD 最小初始化，还未接入 FATFS，也未实现真正的拍照保存流程。

#### 8.12 最终SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

#### 8.13 最终STATUS关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 binary 请求各被阻止一次。`binary_request_error_count=0` 是正确结果，因为 binary 请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=6944
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=72
monitor_heartbeat_age_ms=254
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=248
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=51
uart_dma_rx_bytes=590
stream_buffer_write_bytes=590
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 9. Stage 11C-3板测结论

Stage 11C-3 验证通过。本轮在完整 SDIO GPIO AF12 状态下首次调用 `HAL_SD_Init`，返回 `HAL_OK`，`HAL_SD_GetError` 返回 0，耗时约 4 ms；初始化后 `is_initialized=1`、`sdio_ready=1`、`fatfs_ready=0`。`SD TAKEOVER EXIT` 中 `HAL_SD_DeInit` 返回 `HAL_OK`，并成功关闭 SDIO 时钟、退出完整 SDIO GPIO AF12，同时恢复 PC8、PC9、PC11 为 DCMI AF13。

`SNAPSHOT RESTORE` 后 guard 清除，basic、pc_dump、repeat 20/20 均 PASS，说明最小 `HAL_SD_Init / HAL_SD_DeInit` 流程没有破坏图像链路。最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步。

### 10. 严谨说明

本轮虽然 `HAL_SD_Init` 已成功，但仍未调用 `HAL_SD_ConfigWideBusOperation`，未读取或写入 SD 卡块，未接入 FATFS，未挂载文件系统，也未写入 SD 卡。因此，本轮通过只能说明 SDIO 1-bit 最小 HAL 初始化和反初始化路径可用，不能说明 FATFS 文件写入或 SD 卡拍照保存已经完成。

### 11. 后续Stage 11C-4建议

Stage 11C-4 建议在 `HAL_SD_Init` 成功后调用 `HAL_SD_GetCardInfo` 读取 SD 卡基础信息。下一步仍不接 FATFS、不写卡，只记录：

- `CardType`
- `CardVersion`
- `Class`
- `RelCardAdd`
- `BlockNbr`
- `BlockSize`
- `LogBlockNbr`
- `LogBlockSize`
- `HAL_SD_GetState`
- `HAL_SD_GetCardState`
- `HAL_SD_GetError`

当前工程的 `HAL_SD_CardInfoTypeDef` 不包含 `CardSpeed`、`CardSpeedClass`、`CardCommandClass`，后续实现不得访问这些不存在的字段。

## Stage 11C-4 读取SD卡基础信息

### 1. 本轮目的

- 保持 `SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT` 的安全顺序。
- 在完整 SDIO GPIO AF12 状态下完成 `HAL_SD_Init` 后调用 `HAL_SD_GetCardInfo`。
- 记录 `HAL_SD_GetState`、`HAL_SD_GetCardState` 和读取卡信息后的 `HAL_SD_GetError`。
- 缓存并输出当前 HAL 版本支持的 SD 卡基础字段。
- 新增 `SD CARDINFO` 命令，只打印缓存，不重新初始化或读取卡信息。
- `SD TAKEOVER EXIT` 继续执行 `HAL_SD_DeInit`、关闭 SDIO 时钟和恢复 GPIO，但保留最近一次 card info 缓存。

### 2. 当前HAL实际支持的CardInfo字段

当前 `Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_sd.h` 中的 `HAL_SD_CardInfoTypeDef` 实际包含：

- `CardType`
- `CardVersion`
- `Class`
- `RelCardAdd`
- `BlockNbr`
- `BlockSize`
- `LogBlockNbr`
- `LogBlockSize`

当前结构体不包含 `CardSpeed`、`CardSpeedClass` 或 `CardCommandClass`，本轮代码不定义、不访问这些字段。

### 3. 本轮不做

- 不调用 `HAL_SD_ConfigWideBusOperation`，保持 1-bit 模式和既有 `ClockDiv=118U` 配置。
- 不调用 `HAL_SD_ReadBlocks` 或 `HAL_SD_WriteBlocks`，不读取或写入 SD 卡扇区。
- 不接入 FATFS，不挂载文件系统，不创建或写入文件。
- 不使用 SDIO DMA，不启用 SDIO 中断，不配置 `SDIO_IRQn`。
- 不修改 DCMI、FreeRTOS、IWDG、UART DMA、二进制协议或 OV56RGB5 图像帧格式。

### 4. 新增状态字段

- `card_info_read_attempt_count`
- `card_info_read_success_count`
- `card_info_read_error_count`
- `last_card_info_status`
- `last_card_info_error`
- `last_card_info_operation_ms`
- `last_hal_sd_state`
- `last_hal_sd_card_state`
- `card_type`
- `card_version`
- `card_class`
- `card_rel_card_add`
- `card_block_nbr`
- `card_block_size`
- `card_log_block_nbr`
- `card_log_block_size`

其中 `card_info_read_attempt_count` 只统计实际调用 `HAL_SD_GetCardInfo` 的次数；被 `NEED_TAKEOVER` 阻止或 `HAL_SD_Init` 失败时不增加。卡信息耗时使用出口 tick 减入口 tick，不记录系统绝对 tick。

### 5. 命令和状态行为

- 未完成 TAKEOVER 时，`SD CARDINFO` 只显示全零或最近一次缓存，不触发 `HAL_SD_Init`、`HAL_SD_GetCardInfo` 或任何块读取。
- 前置条件不满足时，`SD INIT` 保持 `NEED_TAKEOVER` 行为，不读取 card info。
- `HAL_SD_Init` 失败时不读取 card info，并保留 EXIT/RESTORE 恢复路径。
- `HAL_SD_Init` 成功后调用 `HAL_SD_GetCardInfo`、`HAL_SD_GetState`、`HAL_SD_GetCardState` 和 `HAL_SD_GetError`。
- card info 成功时输出 `SD INIT: HAL_SD_Init OK, card info OK, FATFS is not mounted.`。
- card info 失败时记录 `CARD_INFO_FAILED`，输出 `SD INIT: HAL_SD_Init OK, card info failed, FATFS is not mounted.`，但不复位、不 FATAL、不卡死。
- `SD CARDINFO`、`SD STATUS`、`SD TAKEOVER STATUS` 均输出 card info 缓存和统计字段。
- `SD TAKEOVER EXIT` 反初始化并关闭 SDIO 时钟后，不清空 card info 缓存；EXIT 后仍可通过 `SD CARDINFO` 查看最近一次结果。
- `fatfs_ready` 始终保持 0；本轮不调用 FATFS 和块读写接口。

### 6. 风险说明

- `HAL_SD_GetCardInfo` 返回成功不等价于 FATFS 可用，只说明 HAL 层识别并提供了卡的基础参数。
- `HAL_SD_GetCardState` 可能返回非传输态，必须原样记录，不能因此复位或进入 FATAL。
- 本轮仍未做块读取、块写入或文件系统挂载，不能据此判断图片保存功能已经完成。
- card info 成功或失败后都必须继续支持 `SD TAKEOVER EXIT`、`SNAPSHOT RESTORE` 和图像链路恢复。

### 7. 后续Stage 11C-5建议

- 如果 card info 读取成功，下一步可进行只读块验证。
- 先读取一个固定扇区到内部小缓冲区，只计算校验值或打印前 16 字节。
- 继续不写卡；写卡和 FATFS 挂载放到后续独立阶段。

### 8. Stage 11C-4板测结果

#### 8.1 启动情况

- 启动正常，`reset: iwdg=0`。
- OV5640 ID 为 `0x5640`，Camera init OK。
- 无 FATAL、无反复复位、无 IWDG 复位循环。

#### 8.2 未初始化前的SD CARDINFO空缓存

依次执行 `SD CARDINFO` 和 `SD STATUS`，结果为：

```text
card_info_read_attempt_count=0
card_info_read_success_count=0
card_info_read_error_count=0
last_card_info_status=0
last_card_info_error=0
last_card_info_operation_ms=0
last_hal_sd_state=0
last_hal_sd_card_state=0
card_type=0
card_version=0
card_class=0
card_rel_card_add=0
card_block_nbr=0
card_block_size=0
card_log_block_nbr=0
card_log_block_size=0
is_initialized=0
sdio_ready=0
fatfs_ready=0
sdio_clock_enabled=0
```

未初始化前 `SD CARDINFO` 只打印缓存，没有触发 `HAL_SD_Init` 或 `HAL_SD_GetCardInfo`，因此 `card_info_read_attempt_count` 仍为 0，符合预期。

#### 8.3 未PREPARE时的SD INIT保护

未执行 `SNAPSHOT PREPARE` 时直接执行 `SD INIT`，输出：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

`SD STATUS` 为：

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=1
init_success_count=0
init_error_count=0
last_error_code=3
last_error_text=NEED_TAKEOVER
takeover_state_text=IDLE
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=0
card_info_read_attempt_count=0
```

该结果说明未执行 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 时，`SD INIT` 被 `NEED_TAKEOVER` 正确阻止；此时没有调用 `HAL_SD_Init`、没有读取 CardInfo，也没有打开 SDIO 时钟。

#### 8.4 SNAPSHOT PREPARE

命令输出：

```text
SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.
```

`SNAPSHOT STATUS` 为：

```text
camera_control_state=2
camera_control_state_text=CAMERA_PAUSED
prepare_attempt_count=1
prepare_success_count=1
real_dcmi_stop_enabled=1
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
last_dcmi_stop_hal_status=0
software_guard_active=1
dump_block_required=1
last_error_code=0
last_error_text=OK
```

`HAL_DCMI_Stop` 调用成功，snapshot guard 已开启。

#### 8.5 完整SDIO GPIO AF12接管

`SD TAKEOVER ENTER` 输出：

```text
SD TAKEOVER ENTER: full SDIO GPIO switched to AF12, run SD INIT next.
```

`SD TAKEOVER STATUS` 为：

```text
takeover_state=1
takeover_state_text=ENTER_DEFERRED
takeover_enter_attempt_count=1
takeover_precheck_attempt_count=1
takeover_precheck_success_count=1
takeover_precheck_fail_count=0
snapshot_pause_confirmed=1
conflict_pin_release_ready=1
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=1
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_af12_selected=1
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
card_info_read_attempt_count=0
```

PC8、PC9、PC11 已先释放并切换到 SDIO AF12，随后 PC8、PC9、PC10、PC11、PC12、PD2 已完整切换到 SDIO AF12。此时尚未调用 `HAL_SD_Init`，所以 `sdio_clock_enabled=0` 和 `card_info_read_attempt_count=0` 是正确状态。

#### 8.6 HAL_SD_Init与CardInfo读取

在完整 SDIO GPIO AF12 状态下执行 `SD INIT`，输出：

```text
SD INIT: HAL_SD_Init OK, card info OK, FATFS is not mounted.
```

`SD STATUS` 关键字段为：

```text
is_initialized=1
takeover_required=1
sdio_ready=1
fatfs_ready=0
init_attempt_count=2
init_success_count=1
init_error_count=0
last_error_code=0
last_error_text=OK
last_operation_ms=5
takeover_state_text=ENTER_DEFERRED
sdio_af12_selected=1
sdio_full_gpio_af12_selected=1
real_hal_sd_init_enabled=1
sdio_clock_enabled=1
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
last_hal_sd_init_status=0
last_hal_sd_error=0
last_sdio_hal_init_operation_ms=4
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
last_card_info_status=0
last_card_info_error=0
last_card_info_operation_ms=0
last_hal_sd_state=1
last_hal_sd_card_state=4
card_type=1
card_version=1
card_class=1461
card_rel_card_add=1
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

`HAL_SD_Init` 和 `HAL_SD_GetCardInfo` 均返回 `HAL_OK`，`HAL_SD_GetError` 返回 0。`card_block_nbr` 与 `card_log_block_nbr` 均为 61022208，`card_block_size` 与 `card_log_block_size` 均为 512，说明基础信息读取成功。`fatfs_ready=0` 是预期结果，本轮没有配置宽总线、没有调用块读写接口、没有接入 FATFS，也没有写文件。

`init_attempt_count=2` 是正确结果：第一次 `SD INIT` 在未 PREPARE、未接管时被 `NEED_TAKEOVER` 阻止，第二次才真正调用 `HAL_SD_Init`。`card_info_read_attempt_count=1` 才代表真实 `HAL_SD_GetCardInfo` 调用次数。

#### 8.7 SD CARDINFO缓存输出

`SD CARDINFO` 输出：

```text
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
last_card_info_status=0
last_card_info_error=0
last_card_info_operation_ms=0
last_hal_sd_state=1
last_hal_sd_card_state=4
card_type=1
card_version=1
card_class=1461
card_rel_card_add=1
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

该命令成功打印缓存，不重新调用 `HAL_SD_Init` 或 `HAL_SD_GetCardInfo`，不读写 SD 卡块，也不接入 FATFS。

#### 8.8 guard状态下的DUMP和binary保护

文本 `DUMP` 输出：

```text
DUMP blocked: snapshot software guard active.
```

此时 `SNAPSHOT STATUS` 中：

```text
software_guard_active=1
dump_block_required=1
dump_block_count=1
binary_block_count=0
```

guard 状态下执行 binary basic，响应长度为 0 B，接收超时，测试结果为 FAIL。该 FAIL 是预期现象；CardInfo 读取成功后 snapshot guard 仍然有效，文本 DUMP 和 binary 图像请求均被阻止，没有输出 OV56RGB5 图像帧。

#### 8.9 SD TAKEOVER EXIT

命令输出：

```text
SD TAKEOVER EXIT: HAL_SD_DeInit status=0, error=0x00000000.
SD TAKEOVER EXIT: full SDIO GPIO restored, conflict pins restored to DCMI AF13.
```

退出后的 `SD STATUS / SD TAKEOVER STATUS` 关键字段为：

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_exit_attempt_count=1
is_initialized=0
sdio_ready=0
fatfs_ready=0
sdio_clock_enabled=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
sdio_af12_selected=0
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
last_sdio_full_gpio_error_text=OK
last_conflict_pin_error_text=OK
```

`HAL_SD_DeInit` 返回 `HAL_OK`，SDIO 时钟已关闭，完整 SDIO GPIO 已退出 AF12，PC8、PC9、PC11 已恢复为 DCMI AF13。EXIT 后 `is_initialized=0`、`sdio_ready=0` 是正确结果，因为已经完成反初始化。`sdio_af12_restore_attempt_count=0` 也是合理结果：当前走完整六引脚退出路径，不调用旧的三冲突引脚退出函数。

#### 8.10 EXIT后的SD CARDINFO缓存

EXIT 后再次执行 `SD CARDINFO`，缓存仍为：

```text
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
card_type=1
card_version=1
card_class=1461
card_rel_card_add=1
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

`SD TAKEOVER EXIT` 没有清空 card info 缓存。即使 `is_initialized=0`、`sdio_ready=0`，仍可查看上一次成功读取到的卡信息。

#### 8.11 SNAPSHOT RESTORE

命令输出：

```text
SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.
```

`SNAPSHOT STATUS` 为：

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

`SNAPSHOT RESTORE` 清除了 snapshot guard，RESTORE 后允许恢复图像导出。

#### 8.12 RESTORE后图像功能回归

- guard 状态下 basic 响应长度为 0 B，测试 FAIL，属于预期现象。
- RESTORE 后 basic 响应长度为 38426 B，`frame_id=1`，CRC 一致，测试 PASS。
- RESTORE 后 pc_dump 测试 PASS，`frame_id=2`，图像质量无阈值警告。
- pc_dump 图像：`captures/015_sd_c4_cardinfo_20260805_200734.png`。
- pc_dump 报告：`captures/015_sd_c4_cardinfo_20260805_200734_report.txt`。
- RESTORE 后 repeat 共请求 20 次，成功 20 次、失败 0 次，成功率 100.00%，测试 PASS。
- repeat 平均耗时 3465.07 ms，最短 3430.19 ms，最长 3470.07 ms。
- repeat 的 `frame_id` 从 3 到 22 连续递增。

#### 8.13 最终SD STATUS

```text
is_initialized=0
takeover_required=1
sdio_ready=0
fatfs_ready=0
init_attempt_count=2
init_success_count=1
init_error_count=0
last_error_code=0
last_error_text=OK
last_operation_ms=5
takeover_state=3
takeover_state_text=EXIT_DEFERRED
sdio_af12_selected=0
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
last_sdio_hal_init_operation_ms=4
last_sdio_hal_deinit_operation_ms=0
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
last_card_info_status=0
last_card_info_error=0
last_card_info_operation_ms=0
last_hal_sd_state=1
last_hal_sd_card_state=4
card_type=1
card_version=1
card_class=1461
card_rel_card_add=1
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

#### 8.14 最终SD TAKEOVER STATUS

```text
takeover_state=3
takeover_state_text=EXIT_DEFERRED
takeover_enter_attempt_count=1
takeover_exit_attempt_count=1
takeover_error_count=0
last_takeover_error_code=6
last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED
takeover_precheck_attempt_count=1
takeover_precheck_success_count=1
takeover_precheck_fail_count=0
snapshot_pause_confirmed=0
conflict_pin_release_ready=0
conflict_pin_release_attempt_count=1
conflict_pin_release_success_count=1
conflict_pin_release_error_count=0
conflict_pin_restore_attempt_count=1
conflict_pin_restore_success_count=1
conflict_pin_restore_error_count=0
conflict_pins_released=0
sdio_af12_switch_attempt_count=1
sdio_af12_switch_success_count=1
sdio_af12_switch_error_count=0
sdio_af12_selected=0
sdio_full_gpio_switch_attempt_count=1
sdio_full_gpio_switch_success_count=1
sdio_full_gpio_switch_error_count=0
sdio_full_gpio_restore_attempt_count=1
sdio_full_gpio_restore_success_count=1
sdio_full_gpio_restore_error_count=0
sdio_full_gpio_af12_selected=0
real_hal_sd_init_enabled=1
sdio_clock_enabled=0
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_init_status=0
last_hal_sd_deinit_status=0
last_hal_sd_error=0
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

`last_takeover_error_text=TAKEOVER_NOT_IMPLEMENTED` 仍是预期结果，因为当前只完成 HAL SD 初始化和基础信息读取，还未接入 FATFS，也未实现真正的拍照保存流程。

#### 8.15 最终SNAPSHOT STATUS

```text
camera_control_state=3
camera_control_state_text=RESTORE_DEFERRED
prepare_attempt_count=1
restore_attempt_count=1
prepare_success_count=1
restore_success_count=0
control_error_count=0
last_error_code=3
last_error_text=CAMERA_RESTORE_NOT_IMPLEMENTED
dcmi_stop_attempt_count=1
dcmi_stop_success_count=1
dcmi_stop_error_count=0
software_guard_active=0
dump_block_required=0
dump_block_count=1
binary_block_count=1
```

#### 8.16 最终STATUS关键字段

`RTOS`：

```text
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_binary_request_seq=20
last_binary_error_code=0
last_error_code=8
```

`dump_error_count=2` 是预期结果，对应 guard 状态下文本 DUMP 和 binary 请求各被阻止一次。`binary_request_error_count=0` 是正确结果，因为 binary 请求帧格式正确，只是被 snapshot guard 拦截，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，属于预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=6736
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`HEARTBEAT`：

```text
camera_service_heartbeat_age_ms=58
monitor_heartbeat_age_ms=533
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=281
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=57
uart_dma_rx_bytes=653
stream_buffer_write_bytes=653
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

### 9. Stage 11C-4板测结论

Stage 11C-4 验证通过。本轮在 `HAL_SD_Init` 成功后调用 `HAL_SD_GetCardInfo`，成功读取到 SD 卡基础信息：`card_block_nbr=61022208`、`card_block_size=512`、`card_log_block_nbr=61022208`、`card_log_block_size=512`。`SD CARDINFO` 能够在初始化前打印空缓存，在读取成功后打印有效缓存，并在 `SD TAKEOVER EXIT` 后继续显示上一次缓存信息。

`SD TAKEOVER EXIT` 中 `HAL_SD_DeInit` 返回 `HAL_OK`，SDIO 时钟关闭，完整 SDIO GPIO 退出 AF12，PC8、PC9、PC11 恢复为 DCMI AF13。`SNAPSHOT RESTORE` 后 basic、pc_dump、repeat 20/20 均 PASS，说明读取 SD 卡基础信息没有破坏图像链路。最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步。

### 10. 严谨说明

本轮虽然成功读取了 SD 卡基础信息，但仍未调用 `HAL_SD_ReadBlocks` 或 `HAL_SD_WriteBlocks`，未接入 FATFS，未挂载文件系统，也未写入 SD 卡。因此，本轮通过只能说明 HAL SD 层已经能够初始化并识别 SD 卡基础参数，不能说明文件系统可用，也不能说明图片保存到 SD 卡已经完成。

### 11. 后续Stage 11C-5建议

Stage 11C-5 建议进行只读块验证，继续不写卡、不接 FATFS。可读取一个固定逻辑块到内部 512 字节缓冲区，例如 block 0 或靠后的安全只读块，只打印前 16 字节和 CRC32/简单校验，不修改 SD 卡内容。只读块验证成功后，再进入后续 FATFS mount；写卡放到更后面的独立阶段。

## Stage 11C-5 只读块验证

### 1. 本轮目的

- 在 `HAL_SD_Init` 和 `HAL_SD_GetCardInfo` 成功后，通过 polling 方式调用 `HAL_SD_ReadBlocks` 只读读取逻辑块 0。
- 每次只读取 1 个 512 B block，使用固定的 512 字节静态缓冲区，不使用动态内存。
- 输出前 16 字节、逐字节 `sum`、逐字节 `xor`、非零字节数、HAL 状态和错误码。
- 新增 `SD READTEST` 命令执行一次只读验证，新增 `SD READINFO` 命令查看最近一次缓存。
- `SD TAKEOVER EXIT` 后保留 read block 缓存，便于在 SDIO 已反初始化且时钟关闭后继续查询。

安全顺序保持为：`SNAPSHOT PREPARE` -> `SD TAKEOVER ENTER` -> `SD INIT` -> `HAL_SD_Init` -> `HAL_SD_GetCardInfo` -> `SD READTEST` -> `HAL_SD_ReadBlocks` -> `SD TAKEOVER EXIT` -> `HAL_SD_DeInit` -> 关闭 SDIO 时钟 -> 恢复 GPIO -> `SNAPSHOT RESTORE`。

### 2. 本轮不做

- 不调用 `HAL_SD_ConfigWideBusOperation`，继续保持 1-bit 和 `ClockDiv=118U`。
- 不调用 `HAL_SD_WriteBlocks`，不修改 SD 卡内容。
- 不接 FATFS，不挂载文件系统，不创建或写入文件。
- 不使用 SDIO DMA，不配置 `SDIO_IRQn`，不启用 SDIO 中断。
- 不修改 DCMI、DCMI DMA、FreeRTOS、IWDG、UART DMA、二进制请求协议或 `OV56RGB5` 图像帧格式。

### 3. 新增状态字段

- `block_read_test_enabled`
- `block_read_attempt_count`
- `block_read_success_count`
- `block_read_error_count`
- `last_block_read_status`
- `last_block_read_error`
- `last_block_read_operation_ms`
- `last_block_read_addr`
- `last_block_read_count`
- `last_block_read_size`
- `last_block_read_sum`
- `last_block_read_xor`
- `last_block_read_nonzero_count`
- `last_block_read_first16`

`block_read_attempt_count` 只统计实际调用 `HAL_SD_ReadBlocks` 的次数。未初始化、SDIO 未就绪、CardInfo 未成功、块大小不支持或地址越界时直接返回 `BLOCK_READ_NOT_READY`，不访问 SD 卡且不增加计数。操作耗时采用出口 tick 减入口 tick，不记录系统绝对 tick。

### 4. 预期命令行为

- 未 INIT 时，`SD READINFO` 只打印缓存，不调用 `HAL_SD_Init`、`HAL_SD_GetCardInfo` 或 `HAL_SD_ReadBlocks`；未读取过时各结果字段为 0。
- 未 INIT 时，`SD READTEST` 返回 not ready，不触发 `HAL_SD_ReadBlocks`。
- 完整 SDIO GPIO 已切换到 AF12 后，`SD INIT` 调用 `HAL_SD_Init` 和 `HAL_SD_GetCardInfo`。
- CardInfo 成功且块大小为 512 B 时，`SD READTEST` 固定调用一次 `HAL_SD_ReadBlocks(..., 0U, 1U, 1000U)`。
- 读取成功后缓存 512 B 统计值和前 16 字节；读取失败时记录 `BLOCK_READ_FAILED`，不复位、不 FATAL、不卡死。
- `SD READINFO` 始终只打印最近一次缓存，不重复读卡。
- `SD STATUS` 和 `SD TAKEOVER STATUS` 增加全部 read block 字段，同时保留原有字段和命令行为。
- `SD TAKEOVER EXIT` 执行 `HAL_SD_DeInit`、关闭 SDIO 时钟并恢复 GPIO，但不清空 card info 或 read block 缓存。
- `SNAPSHOT RESTORE` 后应验证 basic、pc_dump、repeat 20/20 和运行保护状态恢复正常。

### 5. 风险说明

- 本轮首次调用 `HAL_SD_ReadBlocks`，但严格只读 1 个 block，不调用任何写卡 API。
- block 0 可能包含 MBR 或文件系统引导扇区；读取不会修改其内容。
- 单块读取成功不等价于 FATFS 可用，也不代表已经实现图片保存。
- 如果 `HAL_SD_ReadBlocks` 失败，必须保留 HAL 状态与错误码，并保证后续 EXIT/RESTORE 路径仍可执行。
- 卡容量和块参数必须以成功缓存的 CardInfo 为依据；逻辑块大小不是 512 B 时禁止读取。

### 6. 后续Stage 11C-6建议

- 如果单次只读块验证成功，下一步执行多轮 `HAL_SD_Init + CardInfo + ReadBlock + HAL_SD_DeInit + RESTORE` 稳定性验证。
- Stage 11C-6 继续只读、不写卡、不接 FATFS。
- 多轮验证通过后，再考虑独立的 FATFS mount 阶段；挂载和写卡不并入本轮。

## Stage 11C-5A 只读块失败诊断与修正

### 1. Stage 11C-5首次板测现象

- 系统启动正常，`reset: iwdg=0`，OV5640 和 Camera 初始化正常，无 FATAL、无复位循环。
- 未 INIT 时，`SD READINFO` 只显示缓存，`SD READTEST` 返回 not ready，`block_read_attempt_count` 保持 0。
- `SNAPSHOT PREPARE` 后 DCMI stop OK，相机进入 `CAMERA_PAUSED`，软件 guard 生效。
- `SD TAKEOVER ENTER` 成功，`sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=1`。
- `SD INIT` 中 `HAL_SD_Init` 和 CardInfo 均成功；卡逻辑块数量为 61022208，块大小为 512 B，SDIO、初始化和时钟状态均为就绪。
- `HAL_SD_ReadBlocks(block 0, count 1)` 返回失败：`status=1`、`error=0x00000002`；`block_read_attempt_count=1`、`block_read_success_count=0`、`block_read_error_count=1`、`last_block_read_size=0`，前 16 字节全为 `00`。读失败后 card state 从 4 变为 5。
- 失败后的 `SD TAKEOVER EXIT`、`HAL_SD_DeInit`、SDIO 时钟关闭和 GPIO 恢复均成功。`SNAPSHOT RESTORE` 后 basic、pc_dump、repeat 20/20 均 PASS，frame_id 1～22 连续；IWDG、Hook 和 UART RX DMA 状态正常。

因此 Stage 11C-5 的退出与图像恢复路径验证通过，但只读块功能尚未通过，需由 C5A 增强诊断后重新板测。

### 2. 0x00000002错误码含义

当前工程 `stm32f4xx_hal_sd.h` 将 `HAL_SD_ERROR_DATA_CRC_FAIL` 映射到 `SDMMC_ERROR_DATA_CRC_FAIL`，而 `stm32f4xx_ll_sdmmc.h` 明确定义 `SDMMC_ERROR_DATA_CRC_FAIL=0x00000002U`。因此本次 `0x00000002` 的实际错误宏为 `HAL_SD_ERROR_DATA_CRC_FAIL`。

该结果表明当前失败更像数据阶段 CRC 校验失败，而不是命令阶段完全无响应。缓冲区对齐和读前 card state 等待用于排除软件访问条件并增强观测，但在重新板测前不能断言已消除数据 CRC 失败的根因。

### 3. 本轮修正与诊断字段

- 将读缓冲区改为静态 `uint32_t[128]`，总大小仍为 512 B，以保证 4 字节对齐；所有统计和前 16 字节均通过其 `uint8_t` 视图计算。
- 每次读取前清零缓冲区，不使用 malloc、栈缓冲区或 DMA。
- 在调用 `HAL_SD_ReadBlocks` 前，以 1000 ms 超时轮询等待 `HAL_SD_CARD_TRANSFER`，每次循环调用 `HAL_Delay(1U)`。
- 新增等待次数、成功次数、超时次数、等待结束状态与等待耗时字段。
- 新增读前和读后 card state，便于区分进入读操作前是否已处于传输态，以及失败后的状态变化。
- 按本工程 HAL 宏对最近一次读错误执行 bit 判断，记录 DATA CRC、CMD CRC、命令响应超时、数据超时、RX overrun 和 TX underrun 标志。
- `SD READTEST` 支持可选纯十进制 block 地址：无参数默认 block 0，同时支持 `SD READTEST 0` 和 `SD READTEST 2048`。
- 参数非法或逻辑块地址越界时不读卡，也不增加 `block_read_attempt_count`。
- 每次命令仍只调用一次 polling `HAL_SD_ReadBlocks`，且固定读取 1 个 block。
- `SD READINFO`、`SD STATUS` 和 `SD TAKEOVER STATUS` 输出全部新增诊断字段；`SD READINFO` 仍只查看缓存。

新增状态字段如下：

- `block_read_wait_transfer_attempt_count`
- `block_read_wait_transfer_success_count`
- `block_read_wait_transfer_error_count`
- `last_block_read_pre_card_state`
- `last_block_read_post_card_state`
- `last_block_read_wait_card_state`
- `last_block_read_wait_operation_ms`
- `last_block_read_wait_timeout_ms`
- `last_block_read_error_is_data_crc_fail`
- `last_block_read_error_is_cmd_crc_fail`
- `last_block_read_error_is_cmd_rsp_timeout`
- `last_block_read_error_is_data_timeout`
- `last_block_read_error_is_rx_overrun`
- `last_block_read_error_is_tx_underrun`

### 4. 本轮仍不做

- 不调用 `HAL_SD_ConfigWideBusOperation` 或 `HAL_SD_WriteBlocks`。
- 不接 FATFS，不挂载文件系统，不创建或写入文件。
- 不使用 SDIO DMA，不配置或启用 SDIO 中断。
- 不修改 DCMI、DCMI DMA、FreeRTOS、IWDG、UART DMA、协议或图像帧格式。

### 5. 后续板测计划

1. 按既有安全顺序执行 `SNAPSHOT PREPARE`、`SD TAKEOVER ENTER` 和 `SD INIT`。
2. 先执行 `SD READTEST 0` 并用 `SD READINFO` 检查等待状态、读前/读后状态和错误 bit。
3. 如果 block 0 仍失败，再执行 `SD READTEST 2048` 并重新查询缓存。
4. 无论读取成功或失败，都必须执行 `SD TAKEOVER EXIT` 和 `SNAPSHOT RESTORE`。
5. RESTORE 后执行 basic、pc_dump、repeat 20/20，并检查 IWDG、Hook 和 UART RX DMA 状态。

### 6. Stage 11C-5A板测启动与前置保护

板卡启动正常，启动日志显示 `reset: iwdg=0`、`OV5640 ID = 0x5640`、`Camera init OK`；未出现 FATAL、反复复位或 IWDG 复位循环。

未 INIT 时执行 `SD READINFO`，缓存保持初始值：

```text
block_read_test_enabled=1
block_read_attempt_count=0
block_read_success_count=0
block_read_error_count=0
last_block_read_status=0
last_block_read_error=0
last_block_read_operation_ms=0
last_block_read_addr=0
last_block_read_count=0
last_block_read_size=0
last_block_read_sum=0
last_block_read_xor=0
last_block_read_nonzero_count=0
block_read_wait_transfer_attempt_count=0
block_read_wait_transfer_success_count=0
block_read_wait_transfer_error_count=0
last_block_read_pre_card_state=0
last_block_read_post_card_state=0
last_block_read_wait_card_state=0
last_block_read_wait_operation_ms=0
last_block_read_wait_timeout_ms=0
last_block_read_error_is_data_crc_fail=0
last_block_read_first16=00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

此时执行 `SD READTEST` 返回：

```text
SD READTEST: not ready, run SNAPSHOT PREPARE, SD TAKEOVER ENTER and SD INIT first.
```

该命令被正确阻止，没有调用 `HAL_SD_ReadBlocks`，`block_read_attempt_count` 和 `block_read_wait_transfer_attempt_count` 均保持 0。

未执行 PREPARE 时，`SD INIT` 返回：

```text
SD INIT: deferred, need SDIO takeover because PC8/PC9/PC11 conflict with DCMI.
```

状态为 `last_error_text=NEED_TAKEOVER`、`sdio_hal_init_attempt_count=0`、`card_info_read_attempt_count=0`、`block_read_attempt_count=0`、`sdio_clock_enabled=0`。这证明未完成 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 前，不调用 `HAL_SD_Init`、不读取 CardInfo、也不读块。

### 7. PREPARE、SDIO接管与初始化结果

`SNAPSHOT PREPARE` 输出 `SNAPSHOT PREPARE: DCMI stop OK, snapshot software guard active.`，对应状态如下：

```text
camera_control_state_text=CAMERA_PAUSED
prepare_success_count=1
dcmi_stop_success_count=1
software_guard_active=1
dump_block_required=1
```

`SD TAKEOVER ENTER` 输出 `SD TAKEOVER ENTER: full SDIO GPIO switched to AF12, run SD INIT next.`，状态为 `takeover_state_text=ENTER_DEFERRED`、`sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=1`、`sdio_full_gpio_switch_success_count=1`、`sdio_full_gpio_switch_error_count=0`。

随后 `SD INIT` 输出 `SD INIT: HAL_SD_Init OK, card info OK, FATFS is not mounted.`。初始化与卡信息如下：

```text
is_initialized=1
sdio_ready=1
fatfs_ready=0
sdio_clock_enabled=1
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=1
sdio_hal_init_error_count=0
last_hal_sd_init_status=0
last_hal_sd_error=0
card_info_read_attempt_count=1
card_info_read_success_count=1
card_info_read_error_count=0
last_hal_sd_state=1
last_hal_sd_card_state=4
card_type=1
card_version=1
card_class=1461
card_rel_card_add=1
card_block_nbr=61022208
card_block_size=512
card_log_block_nbr=61022208
card_log_block_size=512
```

`HAL_SD_Init` 和 `HAL_SD_GetCardInfo` 均正常，SD 卡基础信息仍可读取；`fatfs_ready=0` 是本阶段未接 FATFS 的预期结果。

### 8. SD READTEST 0结果

命令输出：

```text
SD READTEST: block read failed, block=0, status=1, error=0x00000002.
```

诊断字段如下：

```text
block_read_attempt_count=1
block_read_success_count=0
block_read_error_count=1
last_block_read_status=1
last_block_read_error=2
last_block_read_error_is_data_crc_fail=1
last_block_read_addr=0
last_block_read_count=1
last_block_read_size=0
last_block_read_operation_ms=6
block_read_wait_transfer_attempt_count=1
block_read_wait_transfer_success_count=1
block_read_wait_transfer_error_count=0
last_block_read_pre_card_state=4
last_block_read_post_card_state=4
last_block_read_wait_card_state=4
last_block_read_wait_operation_ms=0
last_block_read_wait_timeout_ms=1000
last_block_read_error_is_cmd_crc_fail=0
last_block_read_error_is_cmd_rsp_timeout=0
last_block_read_error_is_data_timeout=0
last_block_read_error_is_rx_overrun=0
last_block_read_error_is_tx_underrun=0
last_block_read_first16=00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

block 0 读取失败，`0x00000002` 已由当前工程头文件确认对应 `HAL_SD_ERROR_DATA_CRC_FAIL`。读前等待 `HAL_SD_CARD_TRANSFER` 成功，等待结束、读前和读后 card state 均为 4，且 4 字节对齐缓冲已经生效。因此，对齐缓冲和 TRANSFER 等待没有消除 DATA CRC 错误。

### 9. SD READTEST 2048结果

命令输出：

```text
SD READTEST: block read failed, block=2048, status=1, error=0x00000002.
```

诊断字段如下：

```text
block_read_attempt_count=2
block_read_success_count=0
block_read_error_count=2
last_block_read_status=1
last_block_read_error=2
last_block_read_error_is_data_crc_fail=1
last_block_read_addr=2048
last_block_read_count=1
last_block_read_size=0
last_block_read_operation_ms=8
block_read_wait_transfer_attempt_count=2
block_read_wait_transfer_success_count=2
block_read_wait_transfer_error_count=0
last_block_read_pre_card_state=4
last_block_read_post_card_state=4
last_block_read_wait_card_state=4
last_block_read_wait_operation_ms=0
last_block_read_wait_timeout_ms=1000
last_block_read_first16=00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

block 2048 与 block 0 的失败模式一致，均为 `HAL_SD_ERROR_DATA_CRC_FAIL`。这排除了 block 0 特殊区域作为直接原因，问题更可能位于 SDIO 数据阶段的 CRC、采样、时钟、线序、上拉或总线配置。

### 10. Guard、EXIT与缓存保留结果

guard 状态下文本 DUMP 输出 `DUMP blocked: snapshot software guard active.`；此时 `software_guard_active=1`、`dump_block_required=1`、`dump_block_count=1`。guard 状态下 binary basic 收到 0 B 并超时，测试结果 FAIL，这是软件保护阻止图像发送的预期现象；随后 `binary_block_count` 累计为 1。

`SD TAKEOVER EXIT` 输出：

```text
SD TAKEOVER EXIT: HAL_SD_DeInit status=0, error=0x00000000.
SD TAKEOVER EXIT: full SDIO GPIO restored, conflict pins restored to DCMI AF13.
```

退出状态如下：

```text
takeover_state_text=EXIT_DEFERRED
is_initialized=0
sdio_ready=0
fatfs_ready=0
sdio_clock_enabled=0
sdio_hal_deinit_attempt_count=1
sdio_hal_deinit_success_count=1
sdio_hal_deinit_error_count=0
last_hal_sd_deinit_status=0
sdio_full_gpio_af12_selected=0
sdio_af12_selected=0
conflict_pins_released=0
sdio_full_gpio_restore_success_count=1
conflict_pin_restore_success_count=1
```

即使两次读块均失败，`HAL_SD_DeInit`、关闭 SDIO 时钟、完整 SDIO GPIO 退出 AF12，以及 PC8、PC9、PC11 恢复 DCMI AF13 均正常。

EXIT 后 `SD READINFO` 仍保留 `block_read_attempt_count=2`、`block_read_success_count=0`、`block_read_error_count=2`、`last_block_read_status=1`、`last_block_read_error=2`、`last_block_read_error_is_data_crc_fail=1`、`last_block_read_addr=2048`、`last_block_read_count=1`、`last_block_read_size=0` 和全 00 的前 16 字节。`SD CARDINFO` 同样保留一次成功读取的卡信息以及 61022208 个 512 B 逻辑块。

### 11. RESTORE与图像功能回归

`SNAPSHOT RESTORE` 输出 `SNAPSHOT RESTORE: deferred, camera restore and DCMI restart are not implemented yet.`，状态为 `camera_control_state_text=RESTORE_DEFERRED`、`software_guard_active=0`、`dump_block_required=0`、`dump_block_count=1`、`binary_block_count=1`。

RESTORE 后回归结果：

- basic：响应 38426 B，`frame_id=1`，CRC 一致，PASS。
- pc_dump：PASS，`frame_id=2`，图像质量无阈值警告；图像为 `captures/017_sd_c5a_readblock_fix_20260805_210047.png`，报告为 `captures/017_sd_c5a_readblock_fix_20260805_210047_report.txt`。
- repeat：20/20 PASS，成功率 100.00%；平均 3464.62 ms，最短 3423.81 ms，最长 3469.41 ms；frame_id 3～22 连续。

### 12. 最终STATUS关键字段

`RTOS`：

```text
cli_unknown_count=0
dump_request_count=24
dump_success_count=22
dump_error_count=2
binary_request_count=22
binary_request_success_count=21
binary_request_error_count=0
binary_request_crc_error_count=0
binary_request_version_error_count=0
binary_request_type_error_count=0
binary_request_length_error_count=0
binary_request_eof_error_count=0
binary_request_timeout_count=0
last_error_code=8
```

`dump_error_count=2` 对应 guard 状态下文本 DUMP 和 binary 请求各被阻止一次，是预期结果。`binary_request_error_count=0` 表示 binary 请求帧本身正确，只是被 snapshot guard 阻止，不属于协议错误。`last_error_code=8` 对应 snapshot guard active 类错误，也属于预期行为。

`HEALTH`：

```text
camera_service_stack_min_free_bytes=6352
monitor_stack_min_free_bytes=1864
free_heap_bytes=22296
min_ever_free_heap_bytes=22296
```

`HOOK`：

```text
hook_fault_code=0
hook_fault_count=0
assert_line=0
```

`IWDG`：

```text
iwdg_enabled=1
iwdg_refresh_count=279
iwdg_refresh_skip_count=0
iwdg_last_skip_reason=0
iwdg_test_mode=0
```

`UART RX DMA`：

```text
uart_dma_event_count=62
uart_dma_rx_bytes=706
stream_buffer_write_bytes=706
stream_buffer_overflow_bytes=0
uart_dma_error_count=0
uart_dma_recovery_count=0
stream_buffer_resync_count=0
```

最终 STATUS 显示 IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误、无溢出、无恢复、无重同步。

### 13. Stage 11C-5A板测结论

Stage 11C-5A 的失败诊断和系统可恢复性验证完成，但只读块功能仍未通过。当前已确认 `HAL_SD_Init` 和 `HAL_SD_GetCardInfo` 均成功，读块前 card state 已处于 `HAL_SD_CARD_TRANSFER`，读缓冲也已改为 4 字节对齐的 `uint32_t[128]`；然而读取 block 0 和 block 2048 均返回 `HAL_SD_ERROR_DATA_CRC_FAIL`。

由此可以排除未初始化、CardInfo 失败、block 0 特殊区域、读缓冲未对齐，以及读前未进入 TRANSFER 状态作为直接原因。当前问题更可能与 SDIO 数据阶段采样、时钟参数、总线配置、上拉或硬件线序有关。尽管读块失败，EXIT/RESTORE 和图像链路恢复均正常，IWDG、Hook、UART RX DMA 也保持正常。

严谨结论：C5A 不能记录为“只读块验证通过”，只能记录为“只读块失败诊断阶段完成，系统可恢复性通过”。

### 14. 后续Stage 11C-5B建议

下一步继续不进入 FATFS、不写卡，并优先进行 SDIO 时钟参数诊断：

- 保持 1-bit 模式，不启用 SDIO DMA 或中断。
- 支持通过 CLI 查询/设置 SDIO `ClockDiv`，或提供多个只读测试档位。
- 依次测试更低读时钟，例如 `ClockDiv=118`、178、238、255。
- 每个档位重新执行 `SD INIT + CardInfo + SD READTEST 0/2048`，记录 DATA CRC 错误是否消失。
- 如果降低时钟后仍失败，再检查显式 1-bit bus 配置、硬件线序和上拉条件。

## Stage 11C-5B 初版：对照正点原子 polling 读块流程诊断

> 本小节记录初版设计。板测发现将 GPIO speed 改为 HIGH 后 `HAL_SD_Init` 退化失败；当前实现已由后续“Stage 11C-5B 修正版”取代。

### 1. 正点原子参考流程的关键差异

- SDIO GPIO 使用 `GPIO_MODE_AF_PP`、`GPIO_PULLUP`、`GPIO_SPEED_FREQ_HIGH` 和 `GPIO_AF12_SDIO`。
- polling 读写过程通过 `sys_intx_disable()` / `sys_intx_enable()` 避免中断打断 SDIO 数据传输。
- `HAL_SD_ReadBlocks` 返回后继续等待卡重新进入传输完成状态。
- 参考 `sd_init` 在初始化后还会调用 `HAL_SD_ConfigWideBusOperation(..., SDIO_BUS_WIDE_4B)`；本轮继续保持 1-bit，不进入 4-bit 配置。

### 2. 初版目的与实现边界

- 将 PC8、PC9、PC10、PC11、PC12、PD2 切换到 SDIO AF12 时的 GPIO speed 改为 `GPIO_SPEED_FREQ_HIGH`，保持 AF push-pull、上拉和 AF12 不变。
- 增加 `NORMAL` 和 `IRQOFF` 两种 polling 单块读取诊断模式，默认 block 为 0、默认模式为 `NORMAL`。
- `IRQOFF` 模式仅在单次 `HAL_SD_ReadBlocks` 调用期间保存 PRIMASK、关闭全局中断并立即恢复；读前和读后等待、状态采集、延时及 CLI 输出均在中断开启状态执行。
- `HAL_SD_ReadBlocks` 返回后新增 1000 ms 的 `HAL_SD_CARD_TRANSFER` 等待，每 1 ms 轮询一次，并记录成功或超时结果。
- 保留 C5A 的 `uint32_t[128]` 对齐缓冲、读前等待、错误 bit 诊断和可选 block 地址。
- 支持 `SD READTEST`、`SD READTEST 0`、`SD READTEST 2048`，以及显式指定 `NORMAL` 或 `IRQOFF` 的 block 0/2048 组合。
- 每条命令仍只读 1 个 512 B block，不执行任何写卡操作。

新增状态字段如下：

- `sdio_gpio_speed_high_enabled`（初版字段，修正版已移除）
- `block_read_irqoff_supported`
- `last_block_read_mode`
- `block_read_irqoff_attempt_count`
- `block_read_irqoff_success_count`
- `block_read_irqoff_error_count`
- `block_read_post_wait_transfer_attempt_count`
- `block_read_post_wait_transfer_success_count`
- `block_read_post_wait_transfer_error_count`
- `last_block_read_post_wait_card_state`
- `last_block_read_post_wait_operation_ms`
- `last_block_read_post_wait_timeout_ms`

`SD READINFO`、`SD STATUS` 和 `SD TAKEOVER STATUS` 均输出这些字段；`SD READINFO` 仍只读取软件缓存，不调用 `HAL_SD_ReadBlocks`、`HAL_SD_Init` 或 `HAL_SD_GetCardInfo`。

### 3. 命令和参数行为

- `SD READTEST`：block 0，`NORMAL`。
- `SD READTEST 0`：block 0，`NORMAL`。
- `SD READTEST 2048`：block 2048，`NORMAL`。
- `SD READTEST 0 NORMAL` / `SD READTEST 2048 NORMAL`：显式普通 polling 模式。
- `SD READTEST 0 IRQOFF` / `SD READTEST 2048 IRQOFF`：仅在 HAL 读块调用期间关闭全局中断。
- block 地址不是纯十进制时返回 `invalid block address`；模式不是 `NORMAL` 或 `IRQOFF` 时返回 `invalid read mode`；地址越界时返回 `block address out of range`。这些非法参数路径不读卡、不增加普通读块或 IRQOFF 统计。
- 成功和失败输出均包含实际 block 地址与 `NORMAL` / `IRQOFF` 模式。

### 4. 本轮不做

- 不调用 `HAL_SD_ConfigWideBusOperation` 或 `HAL_SD_WriteBlocks`。
- 不接 FATFS，不挂载文件系统，不创建或写入文件。
- 不使用 SDIO DMA，不配置 `SDIO_IRQn`，不启用 SDIO 中断。
- 不修改 DCMI、DCMI DMA、FreeRTOS、IWDG、UART DMA、二进制协议或图像帧格式。

### 5. IRQOFF诊断风险

`IRQOFF` 会在阻塞式 `HAL_SD_ReadBlocks` 执行期间关闭全局中断。该 HAL polling 路径的超时判断依赖系统 tick；全局中断关闭期间 tick 可能无法推进。如果底层调用无法因硬件状态或错误标志自行返回，板卡可能卡在调用中并需要人工复位。因此 `IRQOFF` 只作为对照正点原子流程的短期诊断手段，不作为最终长期方案，也不得扩大到读前/读后等待、串口输出、EXIT 或其他系统路径。

### 6. 后续板测计划

1. 执行 `SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、`SD INIT`，确认 HAL 初始化和 CardInfo 正常。
2. 依次测试 `SD READTEST 0 NORMAL` 和 `SD READTEST 0 IRQOFF`，每次用 `SD READINFO` 保存诊断状态。
3. 如果 block 0 仍失败，再测试 `SD READTEST 2048 NORMAL` 和 `SD READTEST 2048 IRQOFF`。
4. 无论成功或失败，都必须执行 `SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、basic、pc_dump、repeat 20/20，并检查 IWDG、Hook 和 UART RX DMA。

分支判断：

- `NORMAL` 成功：说明 GPIO speed 或当前读块流程修正有效。
- `NORMAL` 失败但 `IRQOFF` 成功：说明 polling 读块可能受到中断打断影响。
- `NORMAL` 和 `IRQOFF` 均失败：进入 C5C，继续诊断 SDIO `ClockDiv`、4-bit 配置、硬件上拉或线序；不得直接进入 FATFS。

## Stage 11C-5B 修正版：恢复 SDIO GPIO speed 为 VERY_HIGH

### 1. 初版C5B板测失败现象

初版 C5B 将 SDIO GPIO speed 从 `GPIO_SPEED_FREQ_VERY_HIGH` 改为 `GPIO_SPEED_FREQ_HIGH`，同时加入 NORMAL/IRQOFF 模式和读后 WaitCardTransfer。板卡启动、未 INIT 保护、`SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 均正常：

- 启动显示 `reset: iwdg=0`、OV5640 OK、Camera init OK。
- 未 INIT 时 `SD READINFO` 正常，`SD READTEST` 正确返回 not ready。
- 未 PREPARE 时 `SD INIT` 正确返回 `NEED_TAKEOVER`。
- PREPARE 后 DCMI stop OK，`software_guard_active=1`。
- TAKEOVER ENTER 后完整 SDIO GPIO 切换到 AF12，`sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=1`。

但是进入接管后的实际 `SD INIT` 失败：

```text
SD INIT: HAL_SD_Init failed, status=1, error=0x00000004.
```

对应状态：

```text
is_initialized=0
sdio_ready=0
fatfs_ready=0
init_attempt_count=2
init_success_count=0
init_error_count=1
last_error_code=18
last_error_text=SDIO_HAL_INIT_FAILED
sdio_clock_enabled=1
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=0
sdio_hal_init_error_count=1
last_hal_sd_init_status=1
last_hal_sd_error=4
card_info_read_attempt_count=0
card_info_read_success_count=0
```

由于 HAL 初始化未成功，`SD READTEST 0 NORMAL`、`0 IRQOFF`、`2048 NORMAL`、`2048 IRQOFF` 均只返回 not ready，没有实际调用 `HAL_SD_ReadBlocks`：`block_read_attempt_count=0`、`block_read_success_count=0`、`block_read_error_count=0`、`block_read_irqoff_attempt_count=0`。

该轮没有进入读块对照阶段。结合 C5A 在 VERY_HIGH 配置下 `HAL_SD_Init` 和 CardInfo 均成功的结果，HIGH speed 是本轮相对于可初始化基线的关键退化项，当前工程不能直接沿用参考代码的 `GPIO_SPEED_FREQ_HIGH` 配置。

### 2. 修正策略

- PC8、PC9、PC10、PC11、PC12、PD2 切换到 SDIO AF12 时全部恢复为 `GPIO_SPEED_FREQ_VERY_HIGH`。
- 保持 `GPIO_PULLUP`、`GPIO_MODE_AF_PP` 和 `GPIO_AF12_SDIO` 不变。
- PC8、PC9、PC11 恢复 DCMI AF13 的 VERY_HIGH 配置和退出 SDIO 流程保持原样。
- 移除不再准确的 `sdio_gpio_speed_high_enabled`，改为 `sdio_gpio_speed_very_high_enabled=1`，CLI 和文档字段与实际 GPIO 配置一致。
- 保留 NORMAL/IRQOFF 读块模式、PRIMASK 保存与恢复、读后 WaitCardTransfer，以及 C5A 的对齐缓冲、读前等待、错误 bit 和可选 block 地址诊断。
- EXIT 后继续保留 CardInfo、读块结果、最近错误、IRQOFF 和 wait transfer 统计缓存。
- 继续只读单个 512 B block，不写卡、不接 FATFS、不使用 SDIO DMA 或 SDIO 中断。

### 3. 后续板测计划

1. 验证启动、未 INIT 保护、`SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER`。
2. 执行 `SD INIT`，首先确认 `HAL_SD_Init` 和 CardInfo 是否恢复 OK。
3. 初始化成功后依次执行 `SD READTEST 0 NORMAL`、`SD READTEST 0 IRQOFF`、`SD READTEST 2048 NORMAL`、`SD READTEST 2048 IRQOFF`，每次使用 `SD READINFO` 保存缓存。
4. 无论成功或失败，都执行 guard DUMP/binary 检查、`SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、basic、pc_dump、repeat 20/20 和最终运行保护检查。

### 4. 后续分支判断

- 恢复 VERY_HIGH 后 `HAL_SD_Init` 重新成功：说明初版 HIGH speed 是初始化退化原因，再继续比较 NORMAL 和 IRQOFF 读块结果。
- NORMAL 失败但 IRQOFF 成功：说明 polling 读块可能受到中断打断影响。
- NORMAL 和 IRQOFF 均失败且仍为 `HAL_SD_ERROR_DATA_CRC_FAIL`：进入 C5C 的 `ClockDiv` 诊断，不直接进入 FATFS。
- 恢复 VERY_HIGH 后 `HAL_SD_Init` 仍失败：先回退 C5B 相关代码，恢复到 C5A 可初始化状态，再继续定位。

## Stage 11C-5R 回退 C5B，恢复 C5A 初始化基线

### 1. C5B修正版板测失败现象

C5B 修正版已将 SDIO GPIO speed 恢复为 VERY_HIGH，并保留 NORMAL/IRQOFF 与读后 WaitCardTransfer。启动、前置保护和接管流程均正常：

- 启动显示 `reset: iwdg=0`，OV5640 和 Camera 初始化正常。
- 未 INIT 时 `SD READINFO` 正常，`sdio_gpio_speed_very_high_enabled=1`，`SD READTEST` 正确返回 not ready，`block_read_attempt_count=0`。
- 未 PREPARE 时 `SD INIT` 正确返回 `NEED_TAKEOVER`。
- `SNAPSHOT PREPARE` 中 DCMI stop OK，`software_guard_active=1`。
- `SD TAKEOVER ENTER` 成功，`sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=1`，两级 GPIO switch success 均为 1。

但接管后的实际 `SD INIT` 仍失败：

```text
SD INIT: HAL_SD_Init failed, status=1, error=0x00000004.
```

关键状态：

```text
is_initialized=0
sdio_ready=0
fatfs_ready=0
init_attempt_count=2
init_success_count=0
init_error_count=1
last_error_code=18
last_error_text=SDIO_HAL_INIT_FAILED
sdio_clock_enabled=1
sdio_hal_init_attempt_count=1
sdio_hal_init_success_count=0
sdio_hal_init_error_count=1
last_hal_sd_init_status=1
last_hal_sd_error=4
card_info_read_attempt_count=0
card_info_read_success_count=0
```

后续所有 NORMAL/IRQOFF、block 0/2048 组合均只返回 not ready，没有实际调用 `HAL_SD_ReadBlocks`：`block_read_attempt_count=0`、`block_read_success_count=0`、`block_read_error_count=0`、`block_read_irqoff_attempt_count=0`。因此 C5B 修正版没有进入读块诊断阶段。

尽管初始化失败，`SD TAKEOVER EXIT`、`HAL_SD_DeInit`、SDIO GPIO 恢复、`SNAPSHOT RESTORE` 均正常，IWDG 未跳过喂狗，Hook 未触发，UART RX DMA 无错误。

### 2. 阶段判断与优先级

C5B 已从 C5A 的“HAL 初始化和 CardInfo 成功、ReadBlocks DATA CRC 失败”退化为 `HAL_SD_Init` 失败，不适合继续扩展读块诊断。当前优先级是先恢复 C5A 已验证的 `HAL_SD_Init OK + HAL_SD_GetCardInfo OK` 基线，再讨论 `HAL_SD_ReadBlocks`。

C5A 的已知边界保持不变：HAL 初始化和 CardInfo 成功；block 0 和 block 2048 的 polling 单块读取均为 `HAL_SD_ERROR_DATA_CRC_FAIL`；EXIT/RESTORE 及图像链路恢复正常。本轮目标不是宣称读块成功，而是消除 C5B 引入的初始化退化变量。

### 3. 本轮回退内容

- 移除 `NORMAL` / `IRQOFF` 模式宏、命令参数、状态字段和统计字段。
- 移除 `__get_PRIMASK`、`__disable_irq`、`__enable_irq` 读块诊断路径。
- 移除 `Camera_SDStorage_WaitCardTransferAfterRead` 及全部 post-wait 状态字段和 CLI 输出。
- 移除仅用于 C5B 区分 GPIO speed 的 `sdio_gpio_speed_very_high_enabled` 字段。
- `SD READTEST` 恢复 C5A 命令形式：无参数默认 block 0，也支持纯十进制 block 地址，例如 0 和 2048；不再接受 NORMAL/IRQOFF 参数。
- 保留静态 `uint32_t[128]` 作为 512 B、4 字节对齐的 polling 读缓冲。
- 保留读前 `Camera_SDStorage_WaitCardTransfer`、读前/读后 card state 和 DATA CRC 等错误 bit 诊断。
- 每次仍只调用一次 `HAL_SD_ReadBlocks`，固定只读 1 个 block。
- SDIO AF12 继续使用 `GPIO_MODE_AF_PP`、`GPIO_PULLUP`、`GPIO_SPEED_FREQ_VERY_HIGH`、`GPIO_AF12_SDIO`。
- EXIT 继续先将完整 SDIO GPIO 退回输入态，再恢复 PC8、PC9、PC11 的 DCMI AF13；不清空 C5A 的 CardInfo 和读块缓存。
- 不写卡、不接 FATFS、不使用 SDIO DMA 或中断、不调用宽总线配置。

本轮 3 个代码文件已恢复到 C5A 提交基线内容，不保留 C5B 源码增量；C5B/C5R 历史仅保留在本文档中。

### 4. 后续板测计划

1. 验证启动以及未 INIT 时 `SD READINFO` / `SD READTEST` 的缓存和 not-ready 保护。
2. 未 PREPARE 时执行 `SD INIT`，确认仍返回 `NEED_TAKEOVER`。
3. 执行 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER`。
4. 再执行 `SD INIT`，重点确认 `HAL_SD_Init` 和 CardInfo 是否恢复 OK。
5. 初始化成功后执行 `SD CARDINFO`、`SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
6. 最后验证 guard DUMP/binary、`SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、basic、pc_dump、repeat 20/20，以及 IWDG、Hook、UART RX DMA。

### 5. 后续分支判断

- C5R 后 `HAL_SD_Init` 恢复 OK：说明 C5B 新增变量导致初始化退化，后续以 C5A/C5R 为基线重新规划读块诊断。
- C5R 后 `HAL_SD_Init` 仍失败：继续逐项比对实际烧录固件、构建产物和 C5A 提交点，必要时直接以 C5A 提交重新构建验证。
- 在 HAL 初始化基线重新确认前，不进入 FATFS、不写卡，也不继续叠加读块实验变量。

## Stage 11C-5C SDIO ClockDiv 诊断

### 1. C5R 基线

- `HAL_SD_Init` 成功，`HAL_SD_GetCardInfo` 成功，卡信息读取正常。
- `SD READTEST 0` 与 `SD READTEST 2048` 均返回 `HAL_SD_ERROR_DATA_CRC_FAIL`。
- `SD TAKEOVER EXIT`、`SNAPSHOT RESTORE` 以及 RESTORE 后的 basic、pc_dump、repeat 20/20 均正常。

### 2. 本轮目的与状态设计

- 新增 `SD CLOCKDIV` 命令，允许在 `SD INIT` 前查询或设置运行时 SDIO `ClockDiv`。
- 默认值保持 C5R 基线的 118，允许范围为 0～255。
- `sdio_clock_div_current` 表示下一次 `HAL_SD_Init` 将使用的值；只有 SD 未初始化且 SDIO 时钟关闭时才允许修改。
- `sdio_clock_div_last_used` 只在真正调用 `HAL_SD_Init` 前更新；前置条件不满足时不更新，初始化失败时仍保留实际尝试值。
- `SD TAKEOVER EXIT` 与 `SNAPSHOT RESTORE` 均不重置 current，只有重新上电或复位恢复默认值 118。
- 计划使用 118、178、238、255 等档位判断读块 `DATA_CRC_FAIL` 是否与 SDIO 时钟有关。

### 3. 本轮保持不变的边界

- 保持 SDIO 1-bit 模式，不调用 `HAL_SD_ConfigWideBusOperation`。
- 不启用 SDIO DMA，不启用 SDIO 中断，不配置 `SDIO_IRQn`。
- 不写卡，不调用 `HAL_SD_WriteBlocks`，不接入或挂载 FATFS。
- 保留 C5A 的 4 字节对齐 `uint32_t[128]` 读缓冲、读前 `WaitCardTransfer` 与错误 bit 诊断。
- 每条 `SD READTEST` 命令仍只读取 1 个 512 B block。
- 不恢复 C5B 的 IRQOFF 模式，也不恢复读后 `WaitCardTransfer`。
- 不修改图像链路、UART DMA、二进制请求协议或 OV56RGB5 帧格式。

### 4. 板测流程

每个 ClockDiv 档位独立执行以下流程：

1. 复位或重新烧录启动。
2. 执行 `SD CLOCKDIV <value>`，再执行 `SD CLOCKDIV` 核对 current 和计数。
3. 未接管前执行 `SD INIT`，确认返回 `NEED_TAKEOVER`，且 last_used 未更新。
4. 执行 `SNAPSHOT PREPARE`。
5. 执行 `SD TAKEOVER ENTER`。
6. 执行 `SD INIT`，确认本次初始化使用的 last_used。
7. 执行 `SD STATUS` 和 `SD CARDINFO`。
8. 若初始化成功，依次执行 `SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
9. 验证 guard 状态下文本 DUMP 被阻止。
10. 执行 `SD TAKEOVER EXIT` 和 `SNAPSHOT RESTORE`。
11. 执行 basic、pc_dump、repeat 20/20，验证图像链路恢复。

本轮 Codex 只完成代码、文档、静态检查与构建，不执行上述硬件板测。

### 5. 预期分支判断

- 如果某个 ClockDiv 下读块成功，保留该档位作为进入 FATFS 前的候选分频，并先完成只读回归验证。
- 如果所有档位下 `HAL_SD_Init` 均成功但读块仍为 `DATA_CRC_FAIL`，说明简单降速未解决问题。
- 如果某个档位下 `HAL_SD_Init` 失败，记录为该档位初始化失败，不将其误判为读块失败。
- 如果所有档位均失败，下一步再考虑 4-bit 对照、显式 bus 配置以及硬件上拉和线序检查。
- 在只读块问题定位完成前，不直接进入 FATFS，不执行写卡。

## Stage 11C-5C ClockDiv 诊断回退结论

### 1. 板测结果

C5C 动态 ClockDiv 方案未推进只读块问题，并使初始化路径出现不稳定和退化：

- `ClockDiv=118`：`HAL_SD_Init failed, status=1, error=0x00000004`。
- `ClockDiv=178`：`HAL_SD_Init failed, status=1, error=0x00000004`。
- `ClockDiv=238`：`HAL_SD_Init failed, status=1, error=0x00000004`。
- `ClockDiv=255` clean test：`HAL_SD_Init failed, status=1, error=0x00000004`。
- CardInfo 未读取，`SD READTEST` 均因 not ready 而未真正执行。
- `SD TAKEOVER EXIT` 和 `SNAPSHOT RESTORE` 正常。
- RESTORE 后 basic、pc_dump、repeat 20/20 均 PASS。
- IWDG、HOOK、UART RX DMA 状态正常。

因此，本轮结果不能用于比较不同 ClockDiv 对 `DATA_CRC_FAIL` 的影响：各档位未能稳定越过 `HAL_SD_Init`，读块路径没有获得有效样本。

### 2. 回退决定

- C5C 动态 ClockDiv 方案不作为有效代码保留。
- 删除 `SD CLOCKDIV` 查询与设置命令、动态配置状态字段、错误码和 Set/Get API。
- `SD INIT` 恢复 C5R 固定 `hsd_snapshot.Init.ClockDiv = 118U` 的初始化路径。
- 保留 C5A/C5R 的 `HAL_SD_Init`、`HAL_SD_GetCardInfo`、`SD READTEST`、`SD READINFO`、4 字节对齐 `uint32_t[128]` 读缓冲、读前 `WaitCardTransfer`、`DATA_CRC_FAIL` 错误 bit 诊断以及单 block 只读路径。
- 不恢复 IRQOFF 或读后 `WaitCardTransfer`，不写卡、不接 FATFS、不启用 SDIO DMA 或 SDIO 中断。

### 3. 后续方向

后续不再继续简单扫描动态 ClockDiv，改为受控验证以下方向：

1. 显式 1-bit / 4-bit bus 配置对照。
2. 在独立阶段受控测试 `HAL_SD_ConfigWideBusOperation`，不得混入本次回退代码。
3. 检查 SDIO CLK、CMD、D0～D3 引脚连接、复用配置和外部/内部上拉条件。
4. 对照正点原子 `sd_init` 的初始化顺序、低速初始化阶段和初始化完成后的总线配置流程。

在新的只读诊断方案验证前，继续保持不写卡、不接 FATFS。
