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
