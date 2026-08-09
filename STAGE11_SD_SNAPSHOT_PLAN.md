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

## Stage 11C-5D 显式 1-bit / 4-bit bus 配置诊断

### 1. 基线与阶段判断

C5R 慢速自动测试已经确认固定 `ClockDiv=118U` 的初始化基线恢复：

- `HAL_SD_Init` 成功，`HAL_SD_GetCardInfo` 成功，逻辑块大小为 512 B。
- `SD READTEST 0` 与 `SD READTEST 2048` 均为 `DATA_CRC_FAIL`，错误码为 2。
- DUMP guard、TAKEOVER EXIT、SNAPSHOT RESTORE 和 basic 图像恢复均 PASS。
- IWDG 未跳过喂狗，HOOK 未触发，UART RX DMA 无错误或 StreamBuffer 溢出。

C5C 动态 ClockDiv 诊断不作为有效代码保留，本阶段继续固定使用 `ClockDiv=118U`。当前问题集中在 `HAL_SD_ReadBlocks` 数据阶段，因此转向显式 bus width 配置对照。

### 2. 本轮目的

- 新增 `SD BUSWIDTH` 状态查询以及 `SD BUSWIDTH 1B`、`SD BUSWIDTH 4B` 命令。
- 在 `HAL_SD_Init` 成功并等待卡进入 `HAL_SD_CARD_TRANSFER` 后，受控调用 `HAL_SD_ConfigWideBusOperation`。
- 分别在显式 1-bit 和 4-bit 配置下执行 block 0、block 2048 单块只读测试，判断 `DATA_CRC_FAIL` 是否与总线宽度配置有关。
- 记录请求宽度、生效宽度、HAL 状态/错误、配置前后 card state、等待状态与耗时。

### 3. 本轮约束

- 不写卡，不调用 `HAL_SD_WriteBlocks`，不接入或挂载 FATFS。
- 不启用 SDIO DMA，不配置或启用 SDIO 中断。
- 不修改图像链路、UART 协议或 OV56RGB5 图像帧格式。
- 不恢复动态 ClockDiv、IRQOFF 或读后 `WaitCardTransfer`。
- 保留 C5A/C5R 的 4 字节对齐 `uint32_t[128]` 缓冲、读前 `WaitCardTransfer`、错误 bit 诊断和每次只读 1 个 block。
- 本轮唯一新增的 SD HAL API 为 `HAL_SD_ConfigWideBusOperation`，且只允许在 `camera_sd_storage.c` 的 `SD BUSWIDTH` 路径调用。

### 4. 手动板测计划

1. 执行 `SD INIT`，确认未接管时返回 `NEED_TAKEOVER`。
2. 执行 `SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、`SD INIT` 和 `SD CARDINFO`。
3. 执行 `SD BUSWIDTH`，确认初始化后的 active width 为 1。
4. 执行 `SD BUSWIDTH 1B`。
5. 依次执行 `SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
6. 执行 `SD BUSWIDTH 4B`。
7. 再次执行 `SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
8. 执行文本 `DUMP`，确认 guard 阻止图像导出。
9. 执行 `SD TAKEOVER EXIT`、`SNAPSHOT RESTORE`、`STATUS` 和 basic 图像恢复测试。

本轮 Codex 不执行硬件测试。后续自动测试计划使用 `tools/uart_sd_buswidth_auto_test.py`，由脚本执行 bus width 对照、readtest、guard、exit、restore 以及 basic/repeat 回归；本轮不新增或修改该 Python 工具。

### 5. 结果判断

- 1B、4B 均为 `DATA_CRC_FAIL`：bus width 配置不是主要原因，下一步检查 SDIO 数据线、上拉和初始化顺序。
- 1B 失败、4B 成功：后续固定使用 4-bit，并先完成进入 FATFS 前的只读稳定性验证。
- 1B 成功、4B 失败：保留 1-bit，后续不启用 4-bit。
- `HAL_SD_ConfigWideBusOperation` 本身失败：记录 HAL status、HAL error 和 card state，不继续该宽度下的 readtest。
- bus width 配置导致卡状态异常，但 EXIT、RESTORE 和 basic 正常：说明安全退出与摄像头恢复机制仍有效。

## Stage 11C-5D-2 最小侵入版 bus width 诊断

### 1. C5D 初版回退原因

C5D 初版在 SD 状态结构、INIT 成功/失败路径以及 EXIT 路径中加入 BUSWIDTH 状态后，尚未真正执行显式 1B/4B 配置，`HAL_SD_Init` 已从 C5R 的稳定成功基线退化为失败。因此初版未获得有效的 1B/4B 对照结果，相关代码已回退，不作为后续基线。

### 2. C5D-2 最小侵入原则

- 不修改 `CameraSdStorageStatus_t`，不扩展 `SD STATUS` 或 `SD TAKEOVER STATUS` 输出。
- 不修改 SD INIT、TAKEOVER ENTER、TAKEOVER EXIT、HAL_SD_DeInit、READTEST 或 READINFO 路径。
- `hsd_snapshot.Init.ClockDiv` 继续固定为 118U，`hsd_snapshot.Init.BusWide` 继续固定为 `SDIO_BUS_WIDE_1B`。
- 不在初始化成功后自动配置 bus width。
- 仅在 `camera_sd_storage.c` 内增加独立静态 debug 状态，并由 `SD BUSWIDTH` 命令单独查询。
- 只有用户在 SD INIT 成功后执行 `SD BUSWIDTH 1B` 或 `SD BUSWIDTH 4B`，才调用一次 `HAL_SD_ConfigWideBusOperation`。
- 继续保持只读：不写卡、不接 FATFS、不使用 SDIO DMA/IRQ，不恢复 CLOCKDIV、IRQOFF 或读后等待。

### 3. 测试流程

1. 执行 `SD INIT`，确认未接管时仍返回 `NEED_TAKEOVER`。
2. 执行 `SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、`SD INIT`、`SD CARDINFO`。
3. 执行 `SD BUSWIDTH`，保存独立 debug 初始状态。
4. 执行 `SD BUSWIDTH 1B`。
5. 执行 `SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
6. 执行 `SD BUSWIDTH 4B`。
7. 再次执行 `SD READTEST 0`、`SD READINFO`、`SD READTEST 2048`、`SD READINFO`。
8. 执行文本 `DUMP`，确认 snapshot guard 生效。
9. 执行 `SD TAKEOVER EXIT`、`SNAPSHOT RESTORE` 和 `STATUS`。
10. 补充 basic 图像恢复验证；无论 bus width 诊断结果如何，EXIT、RESTORE、basic 都必须 PASS。

### 4. 结果判断

- SD INIT 仍 PASS：说明独立静态 debug 状态和 CLI 没有破坏 C5R 初始化主流程。
- 1B、4B 均为 `DATA_CRC_FAIL`：bus width 不是主要原因，后续转向 SDIO 数据线、上拉和初始化顺序检查。
- 1B 失败而 4B 成功：后续可转向 4-bit，并先完成进入 FATFS 前的只读稳定性验证。
- `HAL_SD_ConfigWideBusOperation` 本身失败：记录 debug 状态中的 HAL status、HAL error、配置前后 card state 和等待状态，不把它误判为 READTEST 结果。
- 任一诊断异常后 EXIT、RESTORE 或 basic 失败：必须先恢复 C5R 安全退出和图像恢复基线，不继续扩展 SD 功能。

## Stage 11C-5E SDIO 数据路径 / 硬件线状态诊断

### 1. 当前现象

- C5R 固定 `ClockDiv=118U` 时，`HAL_SD_Init` 和 CardInfo 均成功。
- `HAL_SD_ReadBlocks` 读取 block 0 与 block 2048 均返回 `HAL_SD_ERROR_DATA_CRC_FAIL`（`0x00000002`）。
- C5D-2 最小侵入 BUSWIDTH 诊断没有破坏 SD INIT，但显式 1B、4B 的 `HAL_SD_ConfigWideBusOperation` 均失败，未形成有效的总线宽度读块对照。
- 当前不再继续扫描 ClockDiv 或扩展 BUSWIDTH，诊断重点转向 SDIO GPIO 配置、输入电平和硬件数据路径。

### 2. 本轮目的

- 新增只读命令 `SD LINESTATE`。
- 读取 PC8/PC9/PC10/PC11/PC12 和 PD2 的 MODER、PUPDR、OSPEEDR、AFR 与 IDR。
- 分别输出 D0、D1、D2、D3、CK、CMD 的 mode、pull、speed、AF 和实时输入电平。
- 核对接管后信号线是否实际进入 AF12、Pull-up、Very High 配置。
- 在初始化前后及 READTEST 失败后采样，判断 D0～D3 和 CMD 空闲电平是否为高，是否存在异常低电平或复用配置变化。

### 3. 本轮保持不变

- 不修改 SD INIT、SD READTEST、TAKEOVER ENTER/EXIT、RESTORE 或 GPIO 切换逻辑。
- ClockDiv 继续固定为 118U，初始化 BusWide 继续为 `SDIO_BUS_WIDE_1B`。
- 不新增 `HAL_SD_ConfigWideBusOperation` 调用；仅保留 C5D-2 已有的独立诊断调用。
- 不恢复 CLOCKDIV、IRQOFF 或读后 `WaitCardTransfer`。
- 不写卡、不接 FATFS、不使用 SDIO DMA 或 SDIO IRQ。
- LINESTATE 只读取状态，不修改 SD 计数器、takeover 状态或 snapshot 状态。

### 4. 板测流程

1. 执行 `SD INIT`，确认未接管时返回 `NEED_TAKEOVER`。
2. 执行 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER`。
3. 执行 `SD LINESTATE`，保存 HAL 初始化前的 GPIO 接管状态。
4. 执行 `SD INIT`、`SD STATUS`、`SD CARDINFO` 和 `SD LINESTATE`。
5. 执行 `SD READTEST 0`、`SD READINFO` 和 `SD LINESTATE`。
6. 执行 `SD READTEST 2048`、`SD READINFO` 和 `SD LINESTATE`。
7. 执行文本 `DUMP`，确认 snapshot guard 生效。
8. 执行 `SD TAKEOVER EXIT` 和 `SD LINESTATE`，核对退出后的 GPIO 状态。
9. 执行 `SNAPSHOT RESTORE`、`STATUS`、basic 和 repeat 图像恢复测试。

### 5. 结果判断

- PC8/D0 不是 AF12、Pull-up、Very High 或 IDR 异常：优先检查 D0 线、复用配置和上拉条件。
- PC9、PC10、PC11 即 D1～D3 异常：优先检查 4-bit 相关数据线；1-bit 读块仍应重点关注 D0。
- CMD 状态异常但 CardInfo 稳定成功：需要复核采样时点、外部上拉和命令线连接。
- 全部数据线与 CMD 均为 AF12、Pull-up、Very High 且空闲高，但仍为 `DATA_CRC_FAIL`：下一步检查 SDIO 数据传输参数、卡兼容性、上拉强度或底层 SDIO 数据通路。
- EXIT 后 PC8/PC9/PC11 恢复 DCMI AF13，而 PC10/PC12/PD2 退回输入态：说明 takeover 退出与 GPIO 恢复路径正常。

## Stage 11C-5E-1 修正 SD LINESTATE 只读性

### 1. C5E 初版板测现象

- 在 `SD INIT` 前执行 LINESTATE 时，PC8、PC9、PC10、PC11、PC12、PD2 均已配置为 AF12、Pull-up、Very High，说明 takeover 的 GPIO 切换基本正确。
- 该采样时点下 PC9/D1 与 PC11/D3 的 IDR 为 0，需要在后续数据路径诊断中继续关注。
- 执行 `SD LINESTATE` 后，原本可用的 C5R 初始化基线退化为 `HAL_SD_Init failed`，HAL error 为 `0x00000004`，CardInfo 和 READTEST 未继续执行。
- DUMP guard、TAKEOVER EXIT、SNAPSHOT RESTORE、系统状态和 basic 图像恢复仍正常。
- 检查发现初版 `Camera_SDStorage_PrintLineState` 在 SD ready 时主动调用了 `HAL_SD_GetState` 和 `HAL_SD_GetCardState`；其中 `HAL_SD_GetCardState` 可能访问 SDIO 外设或发送状态命令，不符合 LINESTATE 的严格只读边界。

### 2. C5E-1 修正内容

- `SD LINESTATE` 禁止调用任何 `HAL_SD_*` API。
- `hal_sd_state` 改为输出 `s_camera_sd_status.last_hal_sd_state` 缓存值。
- `hal_sd_card_state` 改为输出 `s_camera_sd_status.last_hal_sd_card_state` 缓存值。
- 新增 `linestate_readonly=1`，声明该命令只读取缓存和 GPIO 寄存器。
- 新增 `linestate_hal_sd_api_call=0`，声明该命令不主动调用 HAL SD API。
- 保留 GPIOC/GPIOD 的 MODER、PUPDR、OSPEEDR、AFR、IDR 原始值以及六根 SDIO 信号线的逐脚解析字段。
- 不修改 SD INIT、READTEST、TAKEOVER ENTER/EXIT、RESTORE、DUMP guard 或 BUSWIDTH 诊断路径。

### 3. 下一次板测

继续使用与 C5E 初版相同的自动脚本和顺序，在 `SD TAKEOVER ENTER` 后、真实 `SD INIT` 前执行 `SD LINESTATE`：

- 如果 `SD_INIT` 恢复 PASS，说明 C5E 初版失败与 LINESTATE 中非纯只读的 HAL SD 状态调用相关。
- 如果 `SD_INIT` 仍 FAIL，需要继续检查 LINESTATE 是否存在其他副作用，或比较 C5E 新增静态数据、日志输出和固件布局对初始化基线的影响。
- 无论初始化结果如何，都必须继续验证 DUMP guard、TAKEOVER EXIT、SNAPSHOT RESTORE、STATUS 和 basic/repeat 恢复路径。

## Stage 11C-5F-1 OV5640 PWDN 物理隔离诊断

### 1. C5E-1 after-init 结果

- `HAL_SD_Init` 与 CardInfo 成功，说明 C5E-1 的严格只读 LINESTATE 没有破坏初始化基线。
- PC8、PC9、PC10、PC11、PC12、PD2 均为 AF12、Pull-up、Very High，GPIO 配置符合预期。
- PC9/SDIO_D1 在初始化后、READTEST 0 后和 READTEST 2048 后的 IDR 均长期为 0；PC8/D0、PC10/D2、PC11/D3 和 PD2/CMD 为高。
- block 0 与 block 2048 仍为 `HAL_SD_ERROR_DATA_CRC_FAIL`。
- TAKEOVER EXIT、SNAPSHOT RESTORE 和 basic 图像恢复正常。

### 2. 本轮假设与目的

PC9 同时连接 SDIO_D1 和 OV5640 DVP 数据线。当前假设是 OV5640 在 DCMI 停止、GPIO 已切换为 SDIO AF12 后仍可能从摄像头侧驱动 PC9 为低。通过 PCF8574 P2 将高有效的 OV5640 PWDN 拉高，使摄像头进入硬件 power-down，再比较 PC9 电平、SD INIT 和 READTEST 结果，以隔离摄像头侧物理驱动影响。

### 3. 本轮实现与安全边界

- 新增 `SD CAMPOWER`，输出独立 PWDN 诊断状态和操作计数。
- 新增 `SD CAMOFF`：仅在 snapshot 已暂停且 SDIO 完整接管后，通过现有 `PCF8574_WriteBit(PCF8574_OV_PWDN_IO, 1)` 拉高 PWDN，并等待 50 ms。
- 新增 `SD CAMON`：通过同一接口将 PWDN 拉低为 0，并等待 50 ms，使 OV5640 退出 power-down。
- CAMOFF 仅是显式诊断命令，不自动加入 TAKEOVER ENTER、SD INIT 或 READTEST。
- TAKEOVER EXIT 前若诊断状态仍为 PWDN active，则先尝试 CAMON 作为安全恢复；即使 CAMON 失败，也继续执行 SDIO DeInit 和 GPIO restore。
- 不修改 SNAPSHOT RESTORE；如果 PWDN power cycle 后图像无法恢复，需要在后续阶段评估是否必须重新初始化 OV5640 或重建 DCMI 采集链路。
- 保持 LINESTATE 严格只读，不恢复 CLOCKDIV、IRQOFF、读后等待，不新增写卡、FATFS、SDIO DMA/IRQ 或第二处 ConfigWideBus 调用。

### 4. 板测流程

1. 执行 `SD INIT`，确认未接管时返回 `NEED_TAKEOVER`。
2. 执行 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER`。
3. 执行 `SD CAMOFF`、`SD CAMPOWER` 和 `SD LINESTATE`。
4. 执行 `SD INIT`、`SD STATUS`、`SD CARDINFO` 和 `SD LINESTATE`。
5. 执行 `SD READTEST 0`、`SD READINFO` 和 `SD LINESTATE`。
6. 执行 `SD READTEST 2048`、`SD READINFO` 和 `SD LINESTATE`。
7. 执行文本 `DUMP`，确认 snapshot guard 生效。
8. 执行 `SD CAMON`、`SD TAKEOVER EXIT` 和 `SNAPSHOT RESTORE`。
9. 执行 `STATUS`、basic 和 repeat 图像恢复测试。

### 5. 结果判断

- CAMOFF 后 PC9/D1 从低变高：PC9 低电平与摄像头侧驱动相关。
- CAMOFF 后 READTEST 成功：`DATA_CRC_FAIL` 根因高度指向 OV5640 DVP 的物理干扰。
- CAMOFF 后 PC9 仍低：低电平可能来自 SD 卡侧、板级电路、SDIO 外设状态或其他冲突。
- CAMOFF 后 PC9 变高但 READTEST 仍为 `DATA_CRC_FAIL`：摄像头确实影响 D1，但读块 CRC 还有其他原因。
- CAMON、EXIT、RESTORE 后 basic 失败：PWDN power cycle 后可能需要重建 OV5640 初始化或 DCMI 采集链路，CAMOFF 不能直接并入正式流程。

## Stage 11C-5G SDIO 读块失败寄存器快照诊断

### 1. 当前现象

- 使用 C5E after-init 流程时，`SD INIT` 与 CardInfo 均可成功。
- `SD READTEST 0` 和 `SD READTEST 2048` 均稳定返回 `HAL_SD_ERROR_DATA_CRC_FAIL`。
- SDIO GPIO 已确认配置为 AF12、Pull-up、Very High，但仍需从 SDIO 外设寄存器层确认数据阶段的实际失败状态。
- C5F 的 OV5640 PWDN 物理隔离不适合并入正式流程，并且会破坏当前相机恢复链路；本轮不再触碰 PWDN、CAMOFF 或 CAMON。

### 2. 本轮目的

- 不改变 `SD READTEST` 的读前等待、单块 polling 读取和错误处理行为。
- 仅在 `HAL_SD_ReadBlocks` 前后采集 SDIO 寄存器快照：读前等待之前、等待成功之后、读块返回之后各一份。
- 仅扩展现有 `SD READINFO`，输出 STA、CLKCR、DCTRL、DLEN、DCOUNT、FIFOCNT、POWER、ARG、CMD、RESPCMD 和读后 RESP1～RESP4。
- 同时记录读前、读后的 HAL state 与读后的 HAL error，并用 STM32F4 的 `SDIO_STA_xxx` 宏解码关键 STA 标志位。
- 快照操作严格只读，不写 `SDIO->ICR`，不清除状态标志。

### 3. 本轮保持不变

- 不修改 SD INIT、SD READTEST 主读取流程、SD TAKEOVER ENTER/EXIT 或 SNAPSHOT RESTORE。
- ClockDiv 继续固定为 `118U`；不恢复动态 CLOCKDIV。
- 保留 C5D-2 既有的一处 `HAL_SD_ConfigWideBusOperation` 独立诊断调用，不新增第二处调用，也不改变 BUSWIDTH 诊断。
- 不恢复 IRQOFF，不增加读后 `WaitCardTransfer`。
- 不写卡、不接 FATFS、不使用 SDIO DMA 或 SDIO IRQ。
- 不触碰摄像头 PWDN、CAMOFF 或 CAMON。
- 不新增 CLI 命令，新字段只由 `SD READINFO` 输出。

### 4. 板测流程

继续使用现有 after-init 线状态脚本：

```text
python tools/uart_sd_line_after_init_auto_test.py --port COM4 --baud 115200 --repeat 0
```

完成 `SNAPSHOT PREPARE`、`SD TAKEOVER ENTER`、`SD INIT`、`SD READTEST 0/2048` 后，再查看 `SD READINFO` 中新增的 before_wait、before_read、after_read 寄存器字段、HAL state/error 和 STA bit 解码字段。仍需完成 TAKEOVER EXIT、SNAPSHOT RESTORE 及图像链路恢复验证。

### 5. 结果判断

- `read_after_sta_dcrc_fail=1` 且 `read_after_sta_dtimeout=0`：说明底层稳定报告数据 CRC 错误，不是数据超时。
- `read_after_sta_dtimeout=1`：重点检查数据线、时钟与采样条件导致的数据超时。
- `read_after_sta_rxoverr=1`：说明 polling 路径可能未及时读取 FIFO；后续再评估节流、DMA 或中断，本阶段不改变读取方式。
- `read_after_sta_stbiterr=1`：说明数据起始位异常，重点检查 D0 线与 SDIO 采样。
- `read_after_read_dcount` 不为 0：说明预期数据长度未完整传输。
- `read_after_read_fifocnt` 异常：说明 FIFO 可能有残留或未正常读出。
- `read_after_sta_dataend` 或 `read_after_sta_dbckend` 与预期不符：说明数据块结束阶段异常。
- `read_hal_error_after_read` 应与底层 STA 快照交叉核对；本轮只收集证据，不据此自动清标志或改变返回逻辑。

## Stage 11C-5H 失败读块 buffer 内容统计

### 1. 当前现象

- `SD INIT` 与 CardInfo 均成功。
- `SD READTEST 0` 和 `SD READTEST 2048` 均返回 `HAL_SD_ERROR_DATA_CRC_FAIL`，失败时 `last_block_read_size=0`。
- C5G 观察到 `HAL_SD_ReadBlocks` 返回后的 `SDIO->STA` 已被 HAL 清零，`read_after_read_sta=0x00000000`，因此无法直接还原失败瞬间的 STA flag。
- C5G 同时记录到 DCTRL=`0x93`、DLEN=512、DCOUNT=0、FIFOCNT=0，说明数据通道按 512 字节配置并结束过，但仍不能判断 512 字节读缓冲区是否被写入。

### 2. 本轮目的

- 在调用 `HAL_SD_ReadBlocks` 前将 512 字节对齐读缓冲区预填为固定模式 `0xA5`。
- `HAL_SD_ReadBlocks` 返回后，无论成功还是失败，均立即统计完整 512 字节缓冲区。
- 记录缓冲区总和、异或值、零值/非零值/`0xFF`/预填值/变化字节计数、变化范围以及 first16、first32、tail16。
- 通过预填模式是否被改变，判断 CRC 失败时数据是否进入内存，以及属于完全未写入、部分写入还是大面积写入。
- 新增的 `buffer_len=512` 仅表示实际检查了 512 字节内存；失败时原有 `last_block_read_size` 继续保持 0，不改变其语义。

### 3. 本轮保持不变

- 不修改 SD INIT、`HAL_SD_ReadBlocks` 调用参数、单次 1 block 读取或原有成功/失败返回逻辑。
- 保持 4 字节对齐 `uint32_t[128]` 缓冲区和读前 `WaitCardTransfer`。
- ClockDiv 继续固定为 `118U`，不改变 BUSWIDTH，不新增 `HAL_SD_ConfigWideBusOperation` 调用。
- 不恢复 IRQOFF 或读后 `WaitCardTransfer`。
- 不写卡、不接 FATFS、不使用 SDIO DMA 或 SDIO IRQ。
- 不触碰摄像头 PWDN、CAMOFF、CAMON、takeover 或 restore 流程。
- 不新增 CLI 命令，仅扩展现有 `SD READINFO`。

### 4. 结果判断

- `last_block_read_buffer_all_prefill=1` 且 `last_block_read_buffer_changed_count512=0`：说明失败时 buffer 未被写入，重点检查 SDIO 接收路径、FIFO 和 HAL polling 读流程。
- `last_block_read_buffer_changed_count512` 大于 0 但明显小于 512：说明只收到部分数据，重点检查数据阶段中断、超时、CRC 或信号线稳定性。
- `last_block_read_buffer_changed_count512` 接近 512：说明数据基本进入 buffer，但 CRC 校验失败，重点检查数据线完整性、SDIO 采样、CRC 校验或总线质量。
- `last_block_read_buffer_all_zero=1`：说明 buffer 被写成全 0，需要结合 first32 和 tail16 判断是有效卡内容还是异常填充。
- `last_block_read_buffer_all_ff=1`：说明 buffer 被写成全 `0xFF`，可能对应空闲线或异常数据模式。
- 没有变化时，first/last changed index 均为 `0xFFFFFFFF`；有变化时分别表示第一个和最后一个非 `0xA5` 字节位置。

### 5. 板测流程

1. 执行：

   ```text
   python tools/uart_sd_line_after_init_auto_test.py --port COM4 --baud 115200 --repeat 0
   ```

2. 查看 `SD READINFO` 中新增的 buffer inspected、长度、预填模式、各类计数、变化范围和 first16/first32/tail16 字段。
3. 完成 TAKEOVER EXIT、SNAPSHOT RESTORE 和 basic 图像恢复验证。
4. 再执行 repeat 20，验证恢复链路稳定性。

## Stage 11C-5J SDIO 数据采样稳定性验证

### 1. 当前现象

- `SD INIT` 与 CardInfo 均可成功。
- `SD READTEST 0` 和 `SD READTEST 2048` 多数返回 `HAL_SD_ERROR_DATA_CRC_FAIL`，但 block 2048 偶尔出现读取 PASS。
- C5H 已证明：返回 `DATA_CRC_FAIL` 时，512 字节 read buffer 仍被完整改写，`changed_count512=512`、first changed index=0、last changed index=511。
- C5I 对同一 block 重复读取发现，多次失败得到的 buffer 指纹并不稳定；block 0 与 block 2048 的 sum512 和 first32 均出现 5 个不同结果，且 block 2048 曾出现一次 PASS。
- 当前现象不是完全未收到数据，也不是固定内容读错，更倾向于 SDIO 数据采样、总线时序或信号质量不稳定。

### 2. 本轮目的

- 不恢复运行时动态 ClockDiv 查询或设置。
- 使用编译期宏 `CAMERA_SD_INIT_CLOCK_DIV` 作为 `hsd_snapshot.Init.ClockDiv` 的唯一配置来源，默认仍为 `118U`。
- 分别将宏固定为 `118U`、`178U`、`238U`、`255U`，每个值单独编译、烧录并断电重上电测试。
- 比较各分频下 SD INIT、CardInfo、block 0/2048 的 READTEST PASS 率、失败 buffer 指纹稳定性以及图像恢复链路。
- `SD STATUS` 与 `SD READINFO` 均输出 `sd_init_clock_div_configured`，用于确认当前烧录固件的编译期配置。

### 3. 本轮保持不变

- SD INIT 仅将 ClockDiv 的来源由硬编码 `118U` 改为编译期宏；ClockEdge、ClockBypass、ClockPowerSave、`SDIO_BUS_WIDE_1B` 和 HardwareFlowControl 均保持不变。
- 不修改 SD READTEST 流程，继续保持读前 `WaitCardTransfer`、单次 1 block polling 读取、`0xA5` 预填、C5G 寄存器快照和 C5H success/fail buffer 统计。
- 不改变 BusWidth，不新增 `HAL_SD_ConfigWideBusOperation` 调用。
- 不恢复 IRQOFF 或读后 `WaitCardTransfer`。
- 不写卡、不接 FATFS、不使用 SDIO DMA 或 SDIO IRQ。
- 不触碰摄像头 PWDN、CAMOFF、CAMON、takeover 或 restore 流程。
- 不通过 CMake 传宏，不新增 CLI、运行时变量或持久化配置。

### 4. 测试矩阵

| CAMERA_SD_INIT_CLOCK_DIV | SD_INIT | CardInfo | BLOCK_0 PASS/Total | BLOCK_2048 PASS/Total | BLOCK_0 指纹稳定性 | BLOCK_2048 指纹稳定性 | BASIC/Repeat | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 118U | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 178U | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 238U | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 255U | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |

每个档位必须使用单独编译的固件，烧录后断电重上电，并使用相同测试脚本、SD 卡、接线和测试顺序；不得通过运行时命令切换分频。

### 5. 结果判断

- ClockDiv 增大后 READTEST PASS 率提高、同一 block 指纹更稳定：根因倾向 SDIO 采样时序或信号质量。
- ClockDiv 增大后 SD INIT 失败：该分频与当前 SD 卡或初始化流程不兼容，不能进入 READTEST 结果比较。
- ClockDiv 增大后仍随机出现 `DATA_CRC_FAIL` 且 buffer 指纹不稳定：继续检查 SDIO 数据线、外部上拉、电源完整性或 HAL polling 读取路径。
- 无论分频结果如何，TAKEOVER EXIT、SNAPSHOT RESTORE、BASIC_IMAGE 和 repeat 都必须继续验证；任一恢复路径失败时，应先恢复图像链路稳定基线。

## Stage 11C-5K 正点原子 SDIO 官方例程对照

### 1. 结论修正

正点原子官方 SDIO 例程已经在同一块阿波罗 F429 开发板验证正常，因此当前不能判断为板载 SDIO 硬件不可用。当前问题应收敛为本项目集成环境与官方 SDIO 例程之间的驱动流程差异、初始化差异、DMA/IRQ 差异、等待逻辑差异或 DCMI/SDIO 复用恢复差异。

本轮用户提供的官方例程路径 `D:\MCU+FreeRTOS\STM32_HAL\ATK_SDIO_EXAMPLE` 实际不存在；在其下一级 `ISP_Project` 目录中找到并扫描了官方例程：`D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ATK_SDIO_EXAMPLE`。以下结论均来自该实际目录。

### 2. 官方例程关键信息表

| 项目 | 正点原子官方例程 | 当前工程 | 差异判断 |
| --- | --- | --- | --- |
| SDIO GPIO | PC8/PC9/PC10/PC11/PC12/PD2，推挽复用 AF12 | PC8/PC9/PC10/PC11/PC12/PD2 切 AF12 | 引脚、模式和复用号一致；当前工程在 takeover 中配置，官方在 `HAL_SD_MspInit` 中配置 |
| SDIO GPIO speed | `GPIO_SPEED_FREQ_HIGH` | `GPIO_SPEED_FREQ_VERY_HIGH` | 存在速度等级差异，应优先按官方 HIGH 做单变量对照 |
| SDIO GPIO pull | `GPIO_PULLUP` | `PULLUP` | 一致 |
| SDIO ClockDiv | `SDIO_TRANSF_CLK_DIV=1`，注释给出的传输时钟为 16 MHz | `CAMERA_SD_INIT_CLOCK_DIV` 默认 `118U` | 重大差异；官方 HAL 初始化后使用快速传输时钟，当前主路径保持低速分频 |
| BusWidth | 句柄先以 1-bit 初始化，随后切换为 4-bit | 当前诊断主路径固定 1-bit | 重大差异；官方稳定路径最终为 4-bit |
| WideBus 配置 | `HAL_SD_ConfigWideBusOperation(..., SDIO_BUS_WIDE_4B)`，失败即返回错误 2 | 当前 BUSWIDTH 1B/4B 诊断失败 | 官方把 4-bit 配置作为初始化必经步骤；当前尚未复现该成功路径 |
| DMA | SDIO 应用路径未配置 DMA，也未调用 Read/Write DMA API | 当前未使用 SDIO DMA | 一致；HAL 驱动虽提供 DMA API，但官方例程没有实际使用 |
| SDIO IRQ | 未配置或启用 `SDIO_IRQn` | 当前未启用 SDIO IRQ | 一致；官方 SDIO 路径为 polling |
| ReadBlocks 调用方式 | `HAL_SD_ReadBlocks` polling，一次可读 `cnt` 个 sector | `HAL_SD_ReadBlocks` polling，每次 1 block | API 模式一致；官方 wrapper 支持多块，当前诊断固定单块 |
| Read 后等待 | 关闭全局中断后调用 ReadBlocks，并循环 `HAL_SD_GetCardState` 等待 `HAL_SD_CARD_TRANSFER`，带 `SD_TIMEOUT` 计数 | 当前主路径未启用读后 WaitCardTransfer | 重大流程差异；官方将读后 CardState 等待纳入完整读事务 |
| FATFS diskio | 本例程未包含 `diskio.c`、`ff.c`、`ffconf.h`、`disk_read` 或 `disk_write`；仅提供标注供 fatfs/usb 调用的 `sd_read_disk`/`sd_write_disk` | 当前未接入 FATFS | 当前均未接入 FATFS；不能从本例程判断官方 diskio 对 DMA/IRQ 的额外依赖 |
| 初始化顺序 | 配置 1-bit 句柄 -> `HAL_SD_Init`（内部调用 `HAL_SD_MspInit`）-> `HAL_SD_GetCardInfo` -> `HAL_SD_ConfigWideBusOperation(4B)` | SNAPSHOT PREPARE -> SD TAKEOVER ENTER -> SD INIT | 当前为解决 DCMI/SDIO 复用而增加 takeover；进入 HAL 后尚未完整对齐官方初始化后半段 |
| 读写保护 | 同时提供读、写 wrapper；polling 读写前关闭全局中断，完成 CardState 等待后恢复中断 | 当前只读，不写卡 | 当前只读边界更严格；但缺少官方 wrapper 的完整临界区和读后等待 |

官方例程的 `HAL_SD_MspInit` 只完成 SDIO 外设时钟、GPIO 端口时钟和六根 AF12/PULLUP/HIGH 引脚配置，没有 SDIO DMA、DMA2 stream、SDIO NVIC 或 `SDIO_IRQn` 配置。官方工程中的 DMA2 关键词来自 LCD DMA2D 等无关路径，不能据此判断 SDIO 使用 DMA。

官方 `sd_read_disk` 和 `sd_write_disk` 的关键事务边界相同：`sys_intx_disable()` -> polling ReadBlocks/WriteBlocks -> 循环等待 CardState 为 TRANSFER -> `sys_intx_enable()`。当前工程只在读前等待 TRANSFER，ReadBlocks 返回后没有同等的 CardState 等待。官方与当前工程使用的 `stm32f4xx_hal_sd.c` 也并非逐字节相同，后续移植诊断分支时需要把 HAL 驱动版本差异作为受控变量记录，不能只复制上层 wrapper。

### 3. 当前工程已知现象

1. SD INIT 能成功。
2. CardInfo 能成功。
3. `HAL_SD_ReadBlocks` 多数返回 `DATA_CRC_FAIL`。
4. C5H 证明 `DATA_CRC_FAIL` 后 512B buffer 已被完整改写。
5. C5I 证明同一 block 多次读出的 buffer 指纹不一致。
6. C5J 证明 `ClockDiv=178U` 有局部改善但不根治，`238U/255U` 初始化失败。
7. 图像链路在 TAKEOVER_EXIT / SNAPSHOT_RESTORE 后可恢复，repeat 20/20 正常。
8. 因此下一阶段不能继续盲调 ClockDiv / PWDN / IRQOFF，而应对照官方例程移植。

### 4. 下一步优先级

第一优先级：

对齐官方 `HAL_SD_MspInit`，包括 GPIO、时钟、DMA、NVIC 配置。当前扫描到的官方实现没有 SDIO DMA/NVIC 配置，因此“对齐”首先意味着集中复现官方 GPIO/时钟路径，并明确保持 SDIO DMA/NVIC 未配置，而不是先行新增 DMA 或 IRQ。

第二优先级：

对齐官方 SD 初始化流程，包括 `HAL_SD_Init`、`HAL_SD_GetCardInfo`、`HAL_SD_ConfigWideBusOperation`、卡状态等待。

第三优先级：

对齐官方 ReadBlocks / WriteBlocks 的等待逻辑、超时逻辑、错误恢复逻辑。

第四优先级：

在当前工程中新增“ATK official SDIO path”诊断分支，先只做初始化、CardInfo、单块读测试，不直接接 FATFS 写文件。

第五优先级：

确认官方例程 diskio 层是否依赖 DMA/IRQ/读后等待，再决定是否接入 FATFS。当前扫描的官方例程不含 diskio/FATFS 源文件，需先取得同版本、同板卡的官方 FATFS 例程，不能从 HAL 驱动声明或 `sd_read_disk` 注释推断依赖。

### 5. 禁止继续盲调

下一阶段不再盲目修改 ClockDiv、PWDN、BusWidth、IRQOFF。所有修改必须来自官方例程差异对照。

## Stage 11C-5L ATK 官方 SDIO init + 4-bit 配置诊断

### 1. 本轮目标

本轮只移植正点原子官方 SDIO 初始化流程，不读取数据块、不写卡。新增独立的 `SD ATKINIT` 与 `SD ATKSTATUS` 诊断命令；ATK 路径不替换、也不修改当前 `SD INIT` 和 `SD READTEST` 主路径。

`SD ATKINIT` 必须在 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 均已完成后执行。未完成完整 SDIO GPIO 接管时返回 `NEED_TAKEOVER`；snapshot 未暂停时返回 `SNAPSHOT_NOT_PAUSED`。

### 2. ATK 官方初始化流程

ATK 诊断路径严格按以下顺序执行：

1. 重新配置 PC8、PC9、PC10、PC11、PC12、PD2。
2. GPIO 使用 `GPIO_MODE_AF_PP`、`GPIO_PULLUP`、`GPIO_SPEED_FREQ_HIGH` 和 `GPIO_AF12_SDIO`。
3. 使能 SDIO 时钟。
4. 使用 `ClockDiv=1U`、1-bit、上升沿、关闭 bypass、关闭 clock power save、关闭 hardware flow control 配置 `hsd_snapshot`。
5. 调用 `HAL_SD_Init`。
6. 初始化成功后调用 `HAL_SD_GetCardInfo`。
7. CardInfo 成功后调用 `HAL_SD_ConfigWideBusOperation(&hsd_snapshot, SDIO_BUS_WIDE_4B)`。
8. 4-bit 配置成功后轮询 `HAL_SD_GetCardState`，等待进入 `HAL_SD_CARD_TRANSFER`。
9. 全部成功后设置 `atk_official_init_ready=1`。

`SD TAKEOVER EXIT` 会识别 ATK 诊断路径，必要时调用 `HAL_SD_DeInit`，关闭 SDIO 时钟并清除 `atk_official_init_ready`，随后继续执行既有 SDIO GPIO 退出和 DCMI AF13 恢复流程。ATK 清理不覆盖当前 SD INIT/READTEST 的缓存结果。

### 3. 诊断状态与输出

`SD ATKSTATUS` 以 `SD ATK:` 为标题，输出 ATK 支持标志、初始化/GPIO/等待计数、HAL init、CardInfo、WideBus 状态与错误、最近卡状态、总耗时、ready、固定 ClockDiv，以及 init 后和 WideBus 后的总线宽度。

`SD STATUS` 同时追加 ATK 摘要，包括 supported、ready、成功/失败计数、HAL init 状态与错误、WideBus 状态与错误、最近卡状态和 ClockDiv。

### 4. 本轮边界

- 不接入 FATFS，不调用 `f_mount`、`f_open`、`f_write` 或 `f_read`。
- 不调用 `HAL_SD_ReadBlocks`、`HAL_SD_WriteBlocks` 或其 DMA 版本。
- 不启用 SDIO DMA 或 SDIO IRQ。
- 不修改当前 READTEST 的读前等待、polling 单块读取或错误处理，不恢复读后 WaitCardTransfer。
- 不恢复动态 `SD CLOCKDIV` CLI 或 IRQOFF。
- 不触碰摄像头 PWDN、CAMOFF 或 CAMON。
- 不修改 DCMI/DMA、协议、FreeRTOS、Python 工具或 CMake 配置。

### 5. 结果判断

- 若 `SD ATKINIT` 全部 PASS，说明官方 init + 4-bit 流程在 takeover 集成环境中可以成立；下一阶段再独立移植官方 polling read。
- 若 `HAL_SD_Init` 失败，优先比较 GPIO speed、ClockDiv 和 takeover 后线状态。
- 若 `HAL_SD_GetCardInfo` 成功但 WideBus 失败，说明 ACMD6 / bus width 配置是当前集成环境的关键差异。
- 若 WideBus 成功但等待 TRANSFER 失败，说明卡状态收敛逻辑需要继续对齐官方等待与超时流程。

本轮 Codex 只执行静态检查和固件构建，不执行硬件测试，不提交 Git commit。



Stage 11C-5L 结论：
在当前 OV5640 + DCMI + FreeRTOS + SD takeover 环境下，ATK 官方参数路径中，1-bit HAL_SD_Init 与 HAL_SD_GetCardInfo 可以成功；但 HAL_SD_ConfigWideBusOperation(SDIO_BUS_WIDE_4B) 失败，错误码为 6，ATK official init_ready 未置位。说明当前集成环境下的关键阻塞点不是 SDIO 初始化本身，而是从 1-bit 切换到 4-bit 的宽总线配置阶段。takeover exit 和 snapshot restore 后图像链路可恢复，basic 与 repeat 20/20 验证通过。

## Stage 11C-5M ATK 官方 1-bit polling read 诊断

### 1. Stage 11C-5L 结论

- ATK 官方参数下，1-bit `HAL_SD_Init` 成功。
- `HAL_SD_GetCardInfo` 成功。
- `HAL_SD_ConfigWideBusOperation(SDIO_BUS_WIDE_4B)` 失败，宽总线切换尚不能成立。
- 因此 Stage 11C-5M 跳过 4-bit 配置，只验证 ATK 官方 1-bit polling read 路径。

### 2. 本轮 ATK1B 路径

1. 在 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 完成后，复用 ATK 官方 GPIO 配置：`GPIO_SPEED_FREQ_HIGH`、`GPIO_PULLUP`、`GPIO_AF12_SDIO`。
2. 固定 `ClockDiv=1U` 和 `SDIO_BUS_WIDE_1B`，依次执行 `HAL_SD_Init`、`HAL_SD_GetCardInfo` 并等待 `HAL_SD_CARD_TRANSFER`。
3. 初始化路径不调用 `HAL_SD_ConfigWideBusOperation`，也不执行读写。
4. `SD ATK1BREAD [block]` 使用 4 字节对齐的静态 512B buffer，读前预填 `0xA5`，每次固定读取 1 block。
5. 保存原始 PRIMASK 后关闭全局中断，以 `HAL_SD_ReadBlocks` polling 方式读取，并在相同临界区内等待卡重新进入 `HAL_SD_CARD_TRANSFER`，最后按原 PRIMASK 恢复中断状态。
6. 无论读取成功或失败，都统计完整 512B buffer 的 sum、xor、分类计数、变化范围及 first16/first32/tail16；失败时 `atk_1bit_last_read_size` 仍为 0。
7. `SD ATK1BSTATUS` 输出独立的初始化、读取、HAL 错误、卡状态、等待和 buffer 诊断字段；`SD STATUS` 只追加 ATK1B 摘要。
8. `SD TAKEOVER EXIT` 对 ATK1B 路径执行 `HAL_SD_DeInit`、关闭 SDIO 时钟并清除 `atk_1bit_init_ready`，随后继续现有 GPIO restore；不清除主路径和 5L 诊断缓存。

### 3. 本轮边界

- 只读 1 block，不写卡。
- 不接入 FATFS，不调用 `f_mount`、`f_open`、`f_write` 或 `f_read`。
- 不使用 SDIO DMA，不启用 SDIO IRQ。
- 不修改现有 `SD INIT`、`SD READTEST` 或 `SD ATKINIT` 4-bit 诊断路径。
- 不触碰摄像头 PWDN、CAMOFF、CAMON，也不恢复动态 ClockDiv CLI。

### 4. 结果判断

- 若 `ATK1BREAD` 稳定 PASS，说明当前失败主要来自现有 READTEST 主路径与官方 polling read 流程的差异。
- 若仍有 `DATA_CRC_FAIL` 但明显比主路径稳定，说明全局中断屏蔽、读后等待或 `ClockDiv=1U` 带来改善。
- 若仍随机出现 `DATA_CRC_FAIL`，说明即使对齐官方 polling read 流程，takeover 环境中的 SDIO 数据线状态恢复仍不稳定。
- 若 `ATK1BINIT` 失败，说明此前观察到的 ATK 官方 1-bit 初始化成功尚不稳定，应先收敛初始化稳定性。

Stage 11C-5M 结论：
ATK 官方 1-bit 初始化路径在当前工程 takeover 环境下可以成立，HAL_SD_Init、CardInfo、TRANSFER 等待均成功。但 ATK 官方 1-bit polling read 路径仍不能稳定读块。block 0 连续出现 DATA_CRC_FAIL，block 2048 出现一次 PASS 后再次 DATA_CRC_FAIL。失败时 512B buffer 被完整改写，说明 SDIO 数据确实进入内存，但数据阶段 CRC 校验仍不稳定。读后等待可将 CardState 恢复到 TRANSFER，但不能消除 DATA_CRC_FAIL。当前问题继续收敛为：相机初始化后的 DCMI/SDIO 共享线 takeover 环境与官方 SDIO 独立例程之间仍存在关键差异。

## Stage 11C-5N ATK1B 连续读块统计

### 1. Stage 11C-5M 板测结论

- `SD ATK1BINIT` 成功，1-bit `HAL_SD_Init`、CardInfo 和 TRANSFER 等待均成立，`ClockDiv=1`、BusWidth=1。
- block 0 两次读取均为 `DATA_CRC_FAIL`，且失败时 512B buffer 被完整改写。
- block 2048 第一次读取 PASS、buffer 全 0，第二次读取为 `DATA_CRC_FAIL`，说明数据阶段是随机不稳定，而非固定地址必错。
- 失败后的读后等待可以使 CardState 返回 `HAL_SD_CARD_TRANSFER`，但 CRC 错误已经发生。
- TAKEOVER_EXIT / SNAPSHOT_RESTORE 后图像链路恢复正常，basic PASS、repeat 20/20 PASS 且 frame_id 连续。

### 2. 本轮范围

Stage 11C-5N 不修改任何固件代码，只新增 `tools/uart_sd_atk1b_repeat_test.py` 自动测试脚本。脚本不执行图像 binary request；图像恢复继续由既有 `tools/uart_image_request_basic.py` 和 `tools/uart_image_request_repeat.py` 单独验证。

### 3. 连续读块测试内容

1. 自动执行 takeover 前 `SD INIT` 阻止检查、`SNAPSHOT PREPARE`、`SD TAKEOVER ENTER` 和 `SD ATK1BINIT`。
2. 对 block 0 连续执行 20 次 `SD ATK1BREAD 0`，每次读后执行 `SD ATK1BSTATUS`。
3. 对 block 2048 连续执行 20 次 `SD ATK1BREAD 2048`，每次读后执行 `SD ATK1BSTATUS`。
4. 逐次分类 `PASS`、`DATA_CRC_FAIL`、`OTHER_FAIL`、`NOT_READY`，并记录 HAL 错误位、CardState、read/wait 耗时和完整 buffer 指纹。
5. 输出 CSV、完整串口 log 和 summary；summary 汇总每个 block 的成功率以及 sum512、first32、tail16、changed/zero/ff count 的唯一值。
6. 无论初始化成功、失败或脚本中途异常，都尽量执行 DUMP guard、`SD TAKEOVER EXIT`、`SNAPSHOT RESTORE` 和 `STATUS`，并保存已经收集的结果。

### 4. 结果判断

- 如果 ATK1B 连续 20 次仍随机出现 `DATA_CRC_FAIL`，说明即使对齐官方 polling read，当前相机初始化加 takeover 环境仍会破坏 SDIO 数据阶段稳定性。
- 如果 ATK1B 成功率明显高于当前 READTEST 主路径，说明官方 GPIO HIGH、`ClockDiv=1`、IRQOFF 和读后等待具有改善价值。
- 如果某个 block 稳定 PASS、另一个 block 不稳定，应继续检查地址、卡内容、读后状态和数据线恢复差异。
- 如果 `ATK1BINIT` 本身不稳定，应先回到初始化稳定性诊断，再比较连续读块结果。

## Stage 11C-5O SD-only 启动环境隔离验证

### 1. Stage 11C-5N 结果

- `ATK1B_INIT=PASS`，`ATK1B_INIT_READY=1`，官方参数 1-bit 初始化路径成立。
- ATK1B polling read 成功率很低：block 0 为 1/20（5.00%），block 2048 为 2/20（10.00%）。
- block 0 的其余 19 次为 `DATA_CRC_FAIL`；block 2048 另有 10 次 `DATA_CRC_FAIL` 和 8 次其他失败。
- 失败读块的 512B buffer 指纹高度随机；block 0 的 `SUM512_UNIQUE=20`，block 2048 的 `SUM512_UNIQUE=12`。

### 2. 本轮目的

- 复现正点原子官方 SDIO 例程的关键环境：启动时不初始化 OV5640，也不启动 DCMI。
- 验证相机未驱动 PC8/PC9/PC11 共享数据线时，SDIO 的 ATK1B 初始化和 polling read 是否稳定。
- `CAMERA_SD_DIAG_SD_ONLY_BOOT=1` 时保留 UART CLI、FreeRTOS 基础任务、SD takeover、`SD ATK1BINIT`、`SD ATK1BREAD` 和 `SD ATK1BSTATUS`，但禁用所有需要相机帧的图像请求。

### 3. 结果判断

- 若 SD-only 模式下 ATK1B repeat 稳定 PASS，根因优先指向 OV5640/DVP 对 PC8/PC9/PC11 共享线的外部驱动或干扰。
- 若 SD-only 模式下仍随机失败，继续对照官方工程检查工程配置、`HAL_SD_MspInit`、HAL 版本与编译配置差异。

### 4. 默认值与测试方法

- `CAMERA_SD_DIAG_SD_ONLY_BOOT` 默认值为 `0U`，正常固件继续使用既有 OV5640、DCMI 和图像链路。
- 板测时手动将宏改为 `1U` 后重新编译、烧录，再执行 takeover 和 ATK1B repeat 测试。
- 测试结束后必须把宏恢复为 `0U`；本轮只完成静态检查和默认配置构建，不执行硬件测试。

## Stage 11C-5O-1 SD-only boot 前置流程修正

### 1. Stage 11C-5O 初测结论

- 初测中 `SNAPSHOT_PREPARE=FAIL`、`TAKEOVER_ENTER=FAIL`、`ATK1B_INIT=FAIL`，因此 `READ_REPEAT=SKIP`，block 0 和 block 2048 均未进入读块阶段。
- 该结果不能用于判断 SD-only 环境下 SDIO 是否稳定。
- 原因是 SD-only 启动没有初始化相机/DCMI，但前置流程仍要求 `SNAPSHOT PREPARE` 和 `SD TAKEOVER ENTER` 满足正常相机 pause 状态。

### 2. 前置流程修正

- SD-only 下 `SNAPSHOT PREPARE` 不调用 DCMI/DMA stop，而是建立虚拟 paused 状态、软件 guard、成功计数及 `last_error_code=0`。
- `SD TAKEOVER ENTER` 只有在 SD-only virtual pause 已建立后才允许切换完整 SDIO GPIO 到 AF12，并继续使用既有 takeover 状态机。
- takeover 完成后，`SD ATK1BINIT` 可继续执行既有 GPIO HIGH/PULLUP/AF12、`ClockDiv=1U`、1-bit、CardInfo 和 TRANSFER 等待流程。
- SD-only 下 DUMP 返回 `DUMP blocked: SD_ONLY_BOOT_NO_CAMERA.`，作为预期的无相机图像请求阻止结果。
- `tools/uart_sd_atk1b_repeat_test.py` 解析 `sd_only_boot`/`sd_only_boot_supported`，并接受 virtual pause 与 `SD_ONLY_BOOT_NO_CAMERA` 文本。

### 3. 保持不变的诊断边界

- 本轮不改变 ATK1B init 的 `ClockDiv=1U`、1-bit 配置，也不改变 ATK1B read 的 `HAL_SD_ReadBlocks` 参数。
- 不接入 FATFS、不写卡、不启用 SDIO DMA 或 SDIO IRQ。
- 不触碰摄像头 PWDN、CAMOFF、CAMON，不恢复动态 SD CLOCKDIV CLI。

## Stage 11C-5O-2 SD-only TAKEOVER ENTER 前置条件修正

### 1. Stage 11C-5O-1 板测结论

- `SD_ONLY_BOOT=1`、`SNAPSHOT_PREPARE=PASS`、`DUMP_GUARD=PASS`，说明 SD-only 启动与 virtual camera pause 已生效。
- `TAKEOVER_ENTER=FAIL`，导致 `ATK1B_INIT=FAIL`、`READ_REPEAT=SKIP`，block 0 和 block 2048 仍未进入读块阶段。
- 这说明 5O-1 的 virtual prepare 尚未完整建立 takeover precheck 所需的软件状态。

### 2. 本轮修正

- SD-only virtual prepare 同时设置 `snapshot_pause_confirmed=1`、`conflict_pin_release_ready=1`、software guard、`CAMERA_PAUSED` 和成功错误码。
- SD-only takeover precheck 只检查上述虚拟暂停与冲突引脚释放许可，不依赖 OV5640 初始化、真实 DCMI/DMA stop、frame buffer 或图像处理链路状态。
- precheck 成功后仍执行 PC8/PC9/PC10/PC11/PC12/PD2 的完整 SDIO AF12 切换，并更新既有 takeover 计数与状态。
- 自动化 summary 新增 takeover 错误码、错误文本、precheck 成功次数、pause/release 状态和 AF12 选择状态；只有原有成功文本出现时才继续 ATK1BINIT，PARTIAL 不视为 PASS。

### 3. 诊断边界与下一步

- 本轮不改变 ATK1B init/read 的 SDIO 参数或 `HAL_SD_ReadBlocks` 参数。
- 仍不接 FATFS、不写卡、不启用 SDIO DMA 或 SDIO IRQ，不触碰摄像头电源控制。
- 下一次板测必须先看到 `TAKEOVER_ENTER=PASS`，才能依据 ATK1BINIT 和 block 0/block 2048 连续读结果判断 SD-only SDIO 稳定性。

## Stage 11C-5O-3 SD-only full SDIO GPIO AF12 switch 失败定位

1. 5O-2 板测已经得到 `SNAPSHOT_PREPARE=PASS`，takeover precheck 也已经 PASS，但 `SD TAKEOVER ENTER` 仍在完整 GPIO 切换阶段返回 `SDIO_FULL_GPIO_SWITCH_FAILED`。
2. 当时 `sdio_af12_selected=1`、`sdio_full_gpio_af12_selected=0`，说明旧的 PC8/PC9/PC11 conflict-pin switch 状态不能代表 PC8/PC9/PC10/PC11/PC12/PD2 完整六线 switch 成功。
3. 根因定位为 SD-only 启动跳过相机/DCMI 初始化，而 `MX_GPIO_Init()` 未开启 GPIOD 时钟；原 full switch 对 PD2 调用 `HAL_GPIO_Init` 后寄存器配置未生效。本轮让 full switch 自行开启 GPIOC/GPIOD 时钟，不再依赖真实相机初始化、DCMI stop、frame buffer 或图像处理链路状态。
4. full switch 仍按现有主路径把 PC8/PC9/PC10/PC11/PC12/PD2 全部配置为 AF12、PULLUP、VERY_HIGH，并在成功时同时设置 `sdio_full_gpio_af12_selected=1` 与 `sdio_af12_selected=1`；失败时保留 `TAKEOVER_ENTER=FAIL`。
5. 新增 `sdio_full_gpio_last_error_pin`，并为六根线分别记录 MODER 的 mode、PUPDR 的 pull、OSPEEDR 的 speed 和 AFRL/AFRH 的 AF 回读值；`SD TAKEOVER`/`SD STATUS` 在失败时也输出这些字段，自动化脚本同步汇总对应大写字段。
6. 本轮不改变 ATK1B init/read 参数或 `HAL_SD_ReadBlocks` 参数，不接 FATFS、不写卡、不启用 SDIO DMA/IRQ，也不触碰摄像头 PWDN、CAMOFF、CAMON 或动态 ClockDiv CLI。

## Stage 11C-5P OV5640 共享 DVP 数据线释放方案查证

### 1. 5O 结论

- 正常 OV5640 + DCMI + FreeRTOS + SD takeover 环境中，ATK1B 初始化可以通过，但 block 0 连续读仅 1/20 PASS，block 2048 连续读仅 2/20 PASS，失败以随机 `DATA_CRC_FAIL` 为主。
- SD-only 环境不初始化 OV5640、不启动 DCMI，完整 SDIO 六线回读均为 AF12/PULLUP/VERY_HIGH；block 0 与 block 2048 各 20 次读取全部通过，即合计 40/40 PASS。
- 因而根因指向正常相机环境下 OV5640/DVP 对 PC8、PC9、PC11 共享数据线的输出未被真正释放，而不是 SDIO 硬件、卡座、SD 卡、`HAL_SD_Init` 或 1-bit polling read 本身。
- 当前 takeover 只改变 STM32 端 GPIO 复用；停止 STM32 DCMI/DMA 也不等价于让 OV5640 端停止驱动 DVP 数据线。

### 2. 当前工程 OV5640 相关代码扫描结果

| 文件名 | 函数名/上下文 | 相关寄存器 | 当前写入值 | 初步含义 | 是否可能用于停止 DVP 输出 |
| --- | --- | --- | --- | --- | --- |
| `BSPDrivers/Inc/OV5640cfg.h` | `ov5640_init_reg_tbl`，由 `OV5640_Min_InitRGB565_QVGA_TestBar()`、`OV5640_Min_InitRGB565_480x320_TestBar()` 及其派生初始化路径写入 | `0x3008` | 表首 `0x42`，表末 `0x02` | 软件掉电/待机后唤醒；当前初始化最终状态是 `0x02` | 是；与 DVP stream stop/on 最直接相关，待验证 |
| `BSPDrivers/Inc/OV5640cfg.h` | `ov5640_init_reg_tbl` | `0x3017` | `0xFF` | FREX、VSYNC、HREF、PCLK、D[9:6] pad output enable | 是；可作为整组 DVP 输出释放候选，待验证 |
| `BSPDrivers/Inc/OV5640cfg.h` | `ov5640_init_reg_tbl` | `0x3018` | `0xFF` | D[5:0]、GPIO[1:0] pad output enable；共享的 OV5640 D2/D3/D4 属于该数据组 | 是；最贴近 PC8/PC9/PC11 三根共享数据线，但精确 mask 与三态行为待验证 |
| `BSPDrivers/Inc/OV5640cfg.h` | 初始化表、JPEG/RGB565 模式表 | `0x3000`、`0x3002`、`0x3004`、`0x3006` | `0x3000=0x00`、`0x3004=0xFF`；JPEG 表 `0x3002=0x00`/`0x3006=0xFF`，RGB565 表 `0x3002=0x1C`/`0x3006=0xC3` | 系统模块 reset/clock enable，现有用法主要控制模块、JFIFO/SFIFO/JPEG 及相关时钟 | 可能间接停止内部数据路径，但不是 pad 三态的直接证据，不作为首选 |
| `BSPDrivers/Inc/OV5640cfg.h`、`BSPDrivers/Src/OV5640.c`、`BSPDrivers/Src/ov5640_tuning.c` | 全文扫描 | `0x3001`、`0x3003`、`0x3005`、`0x3007`、`0x4202` | 未出现 | 当前工程没有这些寄存器的既有实现 | 无现成路径；只能作为新增诊断候选，待验证 |
| `BSPDrivers/Src/OV5640.c`、`BSPDrivers/Inc/OV5640.h` | 公开 OV5640 API | `0x4741` 等 | 测试彩条、尺寸、窗口、格式和画质配置 | 未找到 stream off/on、software standby 或 DVP pad disable/restore 函数 | 否；现有 API 不能释放传感器 DVP 输出 |
| `BSPDrivers/Src/ov5640_tuning.c`、`BSPDrivers/Inc/ov5640_tuning.h` | 曝光、AWB、亮度、对比度、饱和度、锐度 API | AEC/AWB/SDE/CIP 寄存器 | 与输出停止无关 | 只做画质参数读写 | 否 |
| `BSPDrivers/Src/camera_dcmi_dma.c` | `Camera_DCMI_Stop()` | STM32 DCMI/DMA 寄存器 | 清除 capture、禁用 DMA | 只停止 STM32 接收端 | 否；不会向 OV5640 发送 SCCB 命令 |
| `BSPDrivers/Src/camera_snapshot_control.c`、`BSPDrivers/Src/camera_sd_storage.c` | `Camera_SnapshotControl_RequestPrepare()`、full SDIO GPIO switch | 无 OV5640 寄存器写入 | 停止 STM32 采集并切换 MCU GPIO AF | 只处理 STM32 端状态与复用 | 否；这正是正常相机环境仍可能发生总线争用的缺口 |

当前工程没有现成的 OV5640 stream-off、software-standby、DVP-output-disable 或对应 restore API。

### 3. ATK / 正点原子 OV5640 例程扫描结果

本机找到的正点原子相机例程为：

`D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\实验38 摄像头实验\Drivers\BSP\OV5640`

`D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ATK_SDIO_EXAMPLE` 中未找到 OV5640/camera 文件或相关寄存器控制代码。

| 例程路径/文件名 | 函数名/上下文 | 相关寄存器 | 当前写入值 | 初步含义 | 是否可能用于 stream off / standby / DVP disable |
| --- | --- | --- | --- | --- | --- |
| `实验38 摄像头实验/Drivers/BSP/OV5640/ov5640.c` | `ov5640_init()` | `0x3008` | 初始化表前先写 `0x82` | OV5640 软件复位 | 可停止当前输出，但会丢失运行配置，恢复风险高 |
| `实验38 摄像头实验/Drivers/BSP/OV5640/ov5640cfg.h` | `ov5640_init_reg_tbl` | `0x3008` | `0x42` 后最终 `0x02` | software power down/standby 后 wake up | 是；例程只在初始化表中使用，没有独立 stop/restore API |
| 同上 | `ov5640_init_reg_tbl` | `0x3017`、`0x3018` | 均为 `0xFF` | DVP timing/data pad output enable | 是；存在 enable 配置，但没有 disable/tri-state 写法 |
| 同上 | JPEG/RGB565/初始化表 | `0x3000`、`0x3002`、`0x3004`、`0x3006` | 与当前工程相同类型的模块/时钟配置 | 模块 reset 与 clock enable | 可能间接影响输出，但未形成 stream-off 路径 |
| `实验38 摄像头实验/Drivers/BSP/OV5640/ov5640.c` | `ov5640_focus_init()` | `0x3000` | `0x20` 后 `0x00` | reset/release autofocus MCU | 否；只服务 AF 固件初始化，不应当作 DVP 输出控制 |
| `实验38 摄像头实验/Drivers/BSP/OV5640/ov5640.c` | `ov5640_pwdn_set()` | 外部 PWDN | PCF8574 控制 | 硬件掉电 | 已实测破坏恢复链路，禁止作为后续主路线 |
| 正点原子 OV5640 源码全文 | 全文扫描 | `0x4202`、`0x3001`、`0x3003`、`0x3005`、`0x3007` | 未出现 | 没有对应实现证据 | 无现成可移植路径 |

作为语义交叉核对，Linux 主线 OV5640 驱动把 `0x3008` 定义为 `SYS_CTRL0`，把 `0x42/0x02/0x82` 分别定义为 software power-down、power-up、software reset，并在 **DVP** stream stop/on 中写 `0x42/0x02`；同一驱动把 `0x3017/0x3018` 命名为 pad output enable 01/02。其 `0x4202=0x0F/0x00` 位于 **MIPI** stream stop/on 路径，因此不能直接当成本项目 DVP 路径的首选依据。参考：[Linux mainline OV5640 driver](https://github.com/torvalds/linux/blob/master/drivers/media/i2c/ov5640.c)。

### 4. 候选方案列表

- **方案 A：OV5640 stream off 寄存器方案（待验证）**。候选为 `0x4202=0x0F` 停止、`0x4202=0x00` 恢复，但本机当前工程与正点原子例程均未出现该寄存器，且 Linux 主线仅在 MIPI stream 路径使用；对本项目 DVP 接口应降为次级候选，不可直接假定能释放 DVP pad。
- **方案 B：OV5640 software standby 方案（待验证，建议 5Q 第一优先级）**。保存 `0x3008` 原值后，以 `0x42` 停止 DVP stream，再以保存值/已知运行值 `0x02` 恢复。当前工程、正点原子初始化表和 Linux DVP stream 实现三方证据一致，且不同于外部 PWDN；仍必须验证共享线是否真正释放以及图像链路是否完整恢复。
- **方案 C：DVP 输出管脚 disable / tri-state 方案（待验证）**。保存 `0x3017/0x3018` 后关闭相关 pad output enable，再原值恢复。PC8/PC9/PC11 对应 OV5640 D2/D3/D4，均属于 `0x3018` 描述的 D[5:0] 组；在没有确认 bit 映射、无毛刺时序和三态电气行为前，不硬编码清零值或 mask。若方案 B 不能释放共享线，再优先验证该方案。
- **方案 D：sensor soft reset 后不重新启动输出（待验证，最低优先级）**。写 `0x3008=0x82` 可复位 sensor 并停止现有输出，但运行配置可能全部丢失，恢复大概率需要重新执行完整初始化、尺寸、格式、画质与 DCMI 配置；风险明显高于 B/C，只适合作为隔离性诊断，不作为首选恢复方案。
- `0x3000`～`0x3007` 目前只看到模块 reset/clock 控制用法，没有直接证明能够让 DVP pad 三态；暂不单列为主方案，也不通过盲调这些寄存器替代 A～D 的单变量验证。

### 5. 风险判断

- PWDN/CAMOFF 已经验证失败并破坏恢复链路，后续禁止作为主路线，也不以 CAMON 补救该路线。
- 任何停止 OV5640 输出的方案都必须先保存原寄存器值、检查写入/回读结果，并提供严格对称的恢复操作；不能只确认命令返回成功。
- 下一轮硬件验证必须执行完整闭环：停止 OV5640 输出 -> 确认 DCMI/DMA 静止并切换 SDIO -> ATK1B read -> 退出 takeover -> 恢复 OV5640 输出 -> basic/repeat 图像恢复验证。
- 不能直接接 FATFS 或写卡；先用既有只读 ATK1B 单块/连续读确认共享线释放是否消除 CRC 随机失败。
- 不能只证明 SD 能读，还必须证明相机能够恢复，且 basic/repeat 的图像内容、帧连续性和错误计数均正常。
- 方案 A～D 每次只验证一种寄存器路径，禁止把 `0x3008`、`0x4202`、`0x3017/0x3018` 或 reset 混合写入后再归因。

### 6. 下一轮建议

建议下一阶段定义为：**Stage 11C-5Q：OV5640 stream-off / standby 最小诊断 CLI**，候选命令为：

```text
SD SENSORSTOP
SD SENSORRESTORE
SD SENSORSTATUS
```

5Q 只实现最小、可回滚的传感器输出控制与寄存器保存/回读，不接 FATFS、不写卡。建议先单独验证方案 B：在 snapshot prepare 已确认 DCMI/DMA 静止后，保存并回读 `0x3008`，执行 software standby，再进入既有 SD takeover 与 ATK1B read；退出 takeover 后恢复 `0x3008`，等待 sensor 稳定，再执行 basic/repeat 图像验证。若 SD 仍不稳定，再保持相同测试顺序单独验证方案 C；方案 A 与方案 D 分别作为 DVP 适用性较弱和恢复风险较高的后续候选。本轮不实现上述 CLI，也不执行任何 OV5640 寄存器写入或硬件测试。

## Stage 11C-5Q OV5640 0x3008 software standby 最小诊断

### 1. 5O / 5P 结论

- 正常 OV5640 环境中 ATK1B block 0 为 1/20 PASS、block 2048 为 2/20 PASS；SD-only 环境中两个 block 合计 40/40 PASS，说明 SDIO 硬件与 1-bit polling read 本身可用，根因优先指向 OV5640 未释放共享 DVP 数据线。
- 5P 查证确认当前工程与 ATK 初始化表都使用 `0x3008=0x42/0x02`，Linux DVP stream 也使用同一组 software power-down/power-up 值；`0x4202` 属于其 MIPI stream 路径，`0x3017/0x3018` pad output enable 的直接控制风险更高。
- 因此本轮只验证 `0x3008=0x42/0x02`，不混入其他传感器输出控制寄存器，保持单变量归因。

### 2. 最小诊断命令

- `SD SENSORSTOP`：只允许在 `SNAPSHOT PREPARE` 已完成、相机暂停且 software guard 生效后执行；先保存 `0x3008` 原值，再写 `0x42`，等待 20 ms 并读回确认。
- `SD SENSORRESTORE`：只在 SD takeover 已退出后写 `0x3008=0x02`，等待 50 ms 并读回确认；不重新初始化 OV5640、不重写初始化表、不启动 DCMI DMA。
- `SD SENSORSTATUS`：输出 stop/restore 尝试、成功、错误、停止状态、最近错误文本、`0x3008` 写前/写后值和耗时。
- `SD STATUS` 追加 sensor stop 支持、停止状态、stop/restore 成败计数及最近写后回读值摘要。
- `SD TAKEOVER ENTER` 不自动执行 SENSORSTOP，`SD TAKEOVER EXIT` 也不自动执行 SENSORRESTORE。

### 3. 推荐测试流程

```text
SNAPSHOT PREPARE
SD SENSORSTOP
SD SENSORSTATUS
SD TAKEOVER ENTER
SD ATK1BINIT
SD ATK1BREAD 0
SD ATK1BREAD 2048
SD TAKEOVER EXIT
SD SENSORRESTORE
SD SENSORSTATUS
SNAPSHOT RESTORE
basic/repeat 图像恢复
```

连续读稳定性仍应使用既有 ATK1B repeat 测试方法扩大样本；上述两次单块命令先用于确认完整手动流程可执行。

### 4. 结果判断

- 如果 SENSORSTOP 后 ATK1BREAD 明显改善，说明 `0x3008` software standby 能够停止或充分抑制 OV5640 DVP 输出，可继续验证大样本稳定性与恢复闭环。
- 如果 SENSORSTOP 后仍出现随机 CRC_FAIL，说明仅 `0x3008` standby 不足；下一轮再单独考虑 `0x3017/0x3018` pad output enable，不与本轮寄存器混写。
- 如果 SENSORRESTORE 后 basic/repeat 图像不能恢复，说明 `0x3008` 方案恢复风险过高，不能直接并入正式 takeover 主流程。
- 只有 SD 连续读稳定且相机 basic/repeat 恢复均通过，才可认为该候选具备进入后续集成的条件。

### 5. 明确边界

- 禁止 PWDN、CAMOFF、CAMON、PCF8574_P2 路线。
- 不写 `0x4202`，不写 `0x3017/0x3018`，不写 `0x3008=0x82`。
- 不修改 ATK1B init/read 参数、SD INIT 主路径或 SD READTEST 主路径。
- 不接 FATFS、不写卡、不启用 SDIO DMA/IRQ。
- 本轮不执行硬件测试，也不提交 Git commit。

## Stage 11C-5R OV5640 0x3017/0x3018 DVP pad output enable 精确查证

### 1. 5Q 结论回顾

- `SD SENSORSTOP` 已证明可以把 OV5640 `0x3008` 从 `0x02` 写成 `0x42`，寄存器写入与回读路径本身可用。
- 进入 software standby 后，`SD ATK1BINIT` 在 `HAL_SD_Init` 阶段失败，HAL error 为 `0x00000004`，没有进入稳定读块验证。
- `SD SENSORRESTORE` 可以把 `0x3008` 从 `0x42` 写回 `0x02`。
- 写回后图像 `basic/repeat` 均无响应，原图像链路没有恢复。
- 因此 `0x3008` software standby 方案已经失败，禁止并入主流程；后续转向只改变 DVP pad 方向的 `0x3017/0x3018` 最小诊断路径。

### 2. 当前工程 0x3017/0x3018 使用情况

| 文件名 | 函数名或初始化表位置 | 写入值 | 上下文 | 是否默认写入 |
| --- | --- | --- | --- | --- |
| `BSPDrivers/Inc/OV5640cfg.h:209` | `ov5640_init_reg_tbl` | `0x3017=0xFF` | 注释为 FREX、VSYNC、HREF、PCLK、D[9:6] output enable | 是 |
| `BSPDrivers/Inc/OV5640cfg.h:212` | `ov5640_init_reg_tbl` | `0x3018=0xFF` | 注释为 D[5:0]、GPIO[1:0] output enable | 是 |
| `BSPDrivers/Src/OV5640.c` | `OV5640_Min_InitRGB565_QVGA_TestBar()`、`OV5640_Min_InitRGB565_480x320_TestBar()`；real-image 与 160x120 路径复用这些初始化函数 | 通过 `OV5640_Min_WriteTable()` 写入上述初始化表 | 当前 QVGA、160x120、480x320 RGB565 初始化族都会经过该表 | 是 |

结论：当前工程正常完成 OV5640 初始化后，预期运行配置为 `0x3017=0xFF`、`0x3018=0xFF`，即这两个寄存器涉及的 pad 全部设为 output。这里的 `0xFF` 是本工程主动写入的运行值，不是芯片手册给出的寄存器默认值。

### 3. ATK 例程 0x3017/0x3018 使用情况

扫描到的正点原子 OV5640 例程路径为：

`D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\实验38 摄像头实验\Drivers\BSP\OV5640`

| 文件名 | 函数名或初始化表位置 | 写入值 | 上下文 | 与当前工程是否一致 |
| --- | --- | --- | --- | --- |
| `ov5640cfg.h:141` | `ov5640_init_reg_tbl` | `0x3017=0xFF` | FREX、VSYNC、HREF、PCLK、D[9:6] output enable | 是 |
| `ov5640cfg.h:142` | `ov5640_init_reg_tbl` | `0x3018=0xFF` | D[5:0]、GPIO[1:0] output enable | 是 |
| `ov5640.c:103`、`ov5640.c:145-147` | `ov5640_init()` 循环写入 `ov5640_init_reg_tbl` | 写入上述 `0xFF/0xFF` | 完整传感器初始化过程 | 是 |

ATK 例程与当前工程都只给出了全使能写法，没有 DVP data pad disable、保存原值或恢复原值的现成实现；因此下一步只能建立独立、可回滚的最小诊断分支，不能把未验证的固定值直接并入 takeover。

### 4. 0x3017/0x3018 bit 位含义查证

主要证据为 OmniVision `OV5640 color CMOS QSXGA image sensor with OmniBSI technology PRODUCT SPECIFICATION version 2.01` 的 I/O control 表和 system/IO pad control register 表。手册明确说明：I/O control 中 `0=input`、`1=output`；`0x3017[3:0]` 与 `0x3018[7:2]` 依次控制 D[9:0]，两个寄存器的手册默认值均为 `0x00`。Linux 主线驱动也将这两个地址命名为 `OV5640_REG_PAD_OUTPUT_ENABLE01/02`，可作为寄存器用途的实现侧交叉证据。

资料链接：[OV5640 Product Specification PDF](https://files.waveshare.com/upload/d/da/OV5640_DataSheet.pdf)、[Linux mainline OV5640 driver](https://github.com/torvalds/linux/blob/master/drivers/media/i2c/ov5640.c)。

下表“初始值”是芯片手册列出的寄存器默认 bit 值 `0`；当前工程与 ATK 初始化表随后都会主动写成 `1`。

| 寄存器 | bit | 控制对象 | 初始值 | 是否与 DVP 数据线相关 | 是否与 SDIO 冲突线相关 | 证据来源 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x3017` | 7 | FREX output enable | 0 | 否 | 否 | OV5640 Product Specification，PAD OUTPUT ENABLE 01 |
| `0x3017` | 6 | VSYNC output enable | 0 | 否 | 否 | 同上 |
| `0x3017` | 5 | HREF output enable | 0 | 否 | 否 | 同上 |
| `0x3017` | 4 | PCLK output enable | 0 | 否 | 否 | 同上 |
| `0x3017` | 3 | D9 output enable | 0 | 是 | 否，本项目未使用 D9 | 手册的 `Bit[3:0]: D[9:6]` |
| `0x3017` | 2 | D8 output enable | 0 | 是 | 否，本项目未使用 D8 | 同上 |
| `0x3017` | 1 | D7 output enable | 0 | 是 | 否 | 同上 |
| `0x3017` | 0 | D6 output enable | 0 | 是 | 否 | 同上 |
| `0x3018` | 7 | D5 output enable | 0 | 是 | 否 | OV5640 Product Specification，PAD OUTPUT ENABLE 02 的 `Bit[7:2]: D[5:0]` |
| `0x3018` | 6 | **D4 output enable** | 0 | 是 | **是：PC11/SDIO_D3** | 同上 |
| `0x3018` | 5 | **D3 output enable** | 0 | 是 | **是：PC9/SDIO_D1** | 同上 |
| `0x3018` | 4 | **D2 output enable** | 0 | 是 | **是：PC8/SDIO_D0** | 同上 |
| `0x3018` | 3 | D1 output enable | 0 | 是 | 否 | 同上 |
| `0x3018` | 2 | D0 output enable | 0 | 是 | 否 | 同上 |
| `0x3018` | 1 | GPIO1 output enable | 0 | 否 | 否 | OV5640 Product Specification，PAD OUTPUT ENABLE 02 |
| `0x3018` | 0 | GPIO0 output enable | 0 | 否 | 否 | 同上 |

精确结论：OV_D2、OV_D3、OV_D4 分别由 `0x3018[4]`、`0x3018[5]`、`0x3018[6]` 控制；三个位的合并 mask 为 `0x70`。手册能确认写 0 将 pad 方向设为 input，因而它是释放输出驱动的直接候选；但手册没有在该表中单独给出板级共享总线下的高阻、电平和无毛刺保证，实际是否彻底消除 SDIO 干扰仍必须由 5S 板测确认。

### 5. DVP / SDIO 冲突线映射表

| OV5640 DVP 线 | STM32 引脚 | SDIO 功能 | 是否冲突 | 可能受控寄存器 bit |
| --- | --- | --- | --- | --- |
| OV_D0 | PC6 | 无 | 否 | `0x3018[2]` |
| OV_D1 | PC7 | 无 | 否 | `0x3018[3]` |
| OV_D2 | PC8 | SDIO_D0 | 是 | `0x3018[4]` |
| OV_D3 | PC9 | SDIO_D1 | 是 | `0x3018[5]` |
| OV_D4 | PC11 | SDIO_D3 | 是 | `0x3018[6]` |
| OV_D5 | PD3 | 无 | 否 | `0x3018[7]` |
| OV_D6 | PB8 | 无 | 否 | `0x3017[0]` |
| OV_D7 | PB9 | 无 | 否 | `0x3017[1]` |

### 6. 候选 mask 方案

本节只记录候选计算，不实现、不写寄存器。所有方案都必须先通过 SCCB 读取并保存两个寄存器原值，不得假定运行时一定为初始化表中的 `0xFF`。

- **方案 C1：只关闭 OV_D2 / OV_D3 / OV_D4 输出。** 保持 `0x3017` 原值不变；将 `0x3018` 写为 `saved_3018 & ~0x70`，等价于 `saved_3018 & 0x8F`。若实读原值为 `0xFF`，候选写入值才是 `0x8F`。该方案只把 D2/D3/D4 pad 设为 input，保留 D0/D1/D5/D6/D7 以及同步信号输出，是 5S 的第一优先级最小 mask。
- **方案 C2：关闭全部 DVP D0-D7 输出。** 将 `0x3017` 写为 `saved_3017 & ~0x03`，即 `saved_3017 & 0xFC`；将 `0x3018` 写为 `saved_3018 & ~0xFC`，即 `saved_3018 & 0x03`。若两个实读原值均为 `0xFF`，候选值分别为 `0xFC` 与 `0x03`。这会保留 FREX/VSYNC/HREF/PCLK 和 GPIO[1:0] 的原方向，只关闭八根 DVP 数据线；它不是直接写 `0x00`，但影响面仍大于 C1，只应在 C1 不足时单独验证。
- **方案 C3：保存、mask、严格原值恢复。** `DVPSTOP` 先保存 `saved_3017/saved_3018`，确认读成功后才按 C1 或 C2 做 read-modify-write，并逐寄存器回读核对；SDIO 读取完成且 takeover 已退出后，`DVPRESTORE` 将两个寄存器严格恢复为保存值并回读核对。不得用硬编码 `0xFF` 替代保存值，不得只恢复其中一个寄存器，也不得在恢复失败后继续把图像链路报告为正常。

由于 bit 位已经由原厂寄存器表确认，具备进入 5S 最小硬件诊断的资料条件；但“input”在本开发板共享线上的实际释放效果、写寄存器时序以及图像恢复能力仍是待测项，不能把候选 mask 直接视为已验证修复。

### 7. 风险判断

- `0x3017/0x3018` 直接改变 OV5640 pad 输入/输出方向，不执行 sensor reset、software standby 或硬件 PWDN，影响面低于 `0x3008` 与 PWDN；但写错 bit、写错顺序或恢复不完整仍可能破坏图像输出与恢复。
- 下一轮若实现，必须先读取并保存原值，再做 read-modify-write；任一 SCCB 读、写或回读失败都必须中止，不得进入 SDIO takeover。
- 必须提供与 `SENSORRESTORE` 等价的显式 `DVPRESTORE` 命令和状态信息，确保异常路径也能尝试恢复保存值。
- 必须同时验证 SDIO block 0/block 2048 连续读和图像 `basic/repeat`；只有两边都通过，候选方案才成立。
- 禁止直接向 `0x3017` 或 `0x3018` 写 `0x00`；禁止用固定 `0x8F/0xFC/0x03/0xFF` 跳过实读与保存。
- 禁止在 bit 位未确认、寄存器实读失败或保存值无效时操作。
- 禁止 PWDN、CAMOFF、CAMON、PCF8574_P2 路线；禁止回到已失败的 `0x3008` standby 主路线。
- 禁止接入 FATFS、写卡、SDIO DMA 或 SDIO IRQ；先完成只读单块/连续读诊断。

### 8. 下一轮 5S 建议

建议下一阶段定义为：**Stage 11C-5S：OV5640 DVP pad output mask 最小诊断**。

建议 CLI：

```text
SD DVPSTOP
SD DVPRESTORE
SD DVPSTATUS
```

5S 第一轮只实现 C1：保存 `0x3017/0x3018`，仅清除 `0x3018[6:4]`，回读确认后进入既有 SDIO 路径；不同时试 C2，不混写 `0x3008`、`0x4202` 或 PWDN。建议流程：

```text
SNAPSHOT PREPARE
SD DVPSTOP
SD DVPSTATUS
SD TAKEOVER ENTER
SD ATK1BINIT
SD ATK1BREAD 0
SD ATK1BREAD 2048
SD TAKEOVER EXIT
SD DVPRESTORE
SD DVPSTATUS
SNAPSHOT RESTORE
basic/repeat
```

若 C1 下 SDIO 仍不稳定且 DVPRESTORE、`basic/repeat` 均可靠，下一轮才可保持相同流程单独验证 C2。任何 restore 或图像恢复失败都应立即停止扩大测试，不得将 mask 自动并入正式 takeover。

## Stage 11C-5S OV5640 DVP D2/D3/D4 pad output mask 最小诊断

### 1. 5O / 5Q / 5R 结论

- 5O 已证明正常相机环境下 ATK1B block 0 仅 1/20 PASS、block 2048 仅 2/20 PASS，而 SD-only 环境两块合计 40/40 PASS；根因优先指向 OV5640 仍驱动 PC8、PC9、PC11 共享 DVP/SDIO 数据线。
- 5Q 已证明 `0x3008=0x42` software standby 会使 ATK1BINIT 失败，写回 `0x3008=0x02` 后图像 `basic/repeat` 也不能恢复，因此该方案禁止作为主路线。
- 5R 已确认 OV_D2、OV_D3、OV_D4 分别由 `0x3018[4]`、`0x3018[5]`、`0x3018[6]` 控制；当前工程与 ATK 初始化表均写入 `0x3018=0xFF`。

### 2. 本轮最小 mask 范围

本轮只验证 C1，不操作其他 DVP pad。`SD DVPSTOP` 先读取并保存 `0x3018` 原值，再执行 read-modify-write：

```text
new_3018 = old_3018 & 0x8F
```

该 mask 只清除 `0x3018[6:4]`，即只把 OV_D2/PC8/SDIO_D0、OV_D3/PC9/SDIO_D1、OV_D4/PC11/SDIO_D3 的 OV5640 pad 方向切为 input；D0、D1、D5、GPIO[1:0] 以及 `0x3017` 控制的 D6、D7、PCLK、HREF、VSYNC 等均保持不变。实现不会假定原值一定为 `0xFF`，也不会直接写固定 `0x8F`。

### 3. 新增命令与状态

- `SD DVPSTOP`：要求 `SNAPSHOT PREPARE` 已建立相机暂停和 software guard；读取并保存 `0x3018`，写入 `old & 0x8F`，等待 5 ms 后回读核对。重复调用时若 mask 已 active，不覆盖第一次保存的原值。
- `SD DVPRESTORE`：要求已有有效保存值，并要求 SD takeover 已退出；读取当前 `0x3018`，写回保存的原值，等待 5 ms 后回读核对。本命令不重新初始化 OV5640，也不启动 DCMI DMA。
- `SD DVPSTATUS`：输出 mask/restore 尝试、成功、错误计数，active 状态，最近错误码与文本，`0x3018` 保存值、操作前值、写入值、回读值和两类操作耗时。
- `SD STATUS` 追加 DVP mask 支持、active、mask/restore 成败计数以及保存值、mask 后回读值、restore 后回读值摘要。

`SD TAKEOVER ENTER` 不自动调用 DVPSTOP，`SD TAKEOVER EXIT` 也不自动调用 DVPRESTORE；本阶段保持手动顺序，避免把尚未板测的 sensor pad 操作并入正式 takeover。

### 4. DVPSTOP / DVPRESTORE 行为

`DVPSTOP` 的成功条件是：snapshot guard 前置检查通过、SCCB 读取成功、只以 `old & 0x8F` 写入 `0x3018`，并且回读值与写入值完全相等。成功后才设置 `dvp_mask_active=1`；任一步失败均增加错误计数并保留明确错误信息。

`DVPRESTORE` 的成功条件是：`dvp_mask_reg_3018_saved` 有效、takeover 已退出、SCCB 当前值读取成功、保存原值写回成功，并且回读值与保存值完全相等。成功后才清除 `dvp_mask_active`。没有保存值时返回 `NO_SAVED_3018`，takeover 仍 active 时拒绝恢复。

### 5. 推荐板测流程

```text
SNAPSHOT PREPARE
SD DVPSTATUS
SD DVPSTOP
SD DVPSTATUS
SD TAKEOVER ENTER
SD ATK1BINIT
SD ATK1BREAD 0
SD ATK1BREAD 2048
SD ATK1BSTATUS
SD TAKEOVER EXIT
SD DVPRESTORE
SD DVPSTATUS
SNAPSHOT RESTORE
STATUS
basic/repeat 图像恢复
```

本轮 Codex 不执行上述硬件测试；该流程用于下一次开发板验证。

### 6. 结果判断

- 如果 DVPSTOP 后 ATK1BINIT/READ 明显改善，并且 DVPRESTORE 后图像 `basic/repeat` 恢复，说明 `0x3018[6:4]` 是有效的最小共享线释放方案，可进入后续大样本稳定性验证。
- 如果 DVPSTOP 后仍以随机 `DATA_CRC_FAIL` 为主，说明只释放 D2/D3/D4 不足；确认恢复闭环可靠后，再单独规划 C2 或其他方案。
- 如果 DVPRESTORE 后图像不能恢复，说明该 pad mask 路径恢复风险仍高，不能自动并入 snapshot/takeover 主流程。

### 7. 明确禁止

- 禁止直接写 `0x3018=0x00`，禁止关闭全部 DVP D0-D7。
- 禁止写 `0x3017`、`0x3008` 或 `0x4202`。
- 禁止 PWDN、CAMOFF、CAMON、PCF8574_P2 路线。
- 禁止接入 FATFS、写卡、`HAL_SD_WriteBlocks`、SDIO DMA 或 SDIO IRQ。
- 禁止修改 ATK1B init/read 参数、SD INIT 主路径或 SD READTEST 主路径。
- 禁止自动调用 DVPSTOP/DVPRESTORE，禁止执行硬件复位或系统复位。

## Stage 11C-5T DVPSTOP 后 ATK1B 连续读块统计

### 1. Stage 11C-5S 手动验证结果

- `SD DVPSTOP` 成功将 OV5640 `0x3018` 从 `0xFF` 改为 `0x8F`，回读同为 `0x8F`；只清除了控制 OV_D2、OV_D3、OV_D4 的 `0x3018[6:4]`。
- 完整 SDIO GPIO takeover 成功，PC8、PC9、PC10、PC11、PC12、PD2 均切换为 AF12。
- `SD ATK1BINIT` 成功，HAL_SD_Init、CardInfo 和 card TRANSFER 等待均通过。
- 单次 `SD ATK1BREAD 0` 与 `SD ATK1BREAD 2048` 均 PASS。
- `SD DVPRESTORE` 成功将 `0x3018` 从 `0x8F` 恢复到保存的 `0xFF`，回读为 `0xFF`。
- 图像 basic PASS，repeat 20/20 PASS，frame_id 连续；C1 同时具备 SDIO 改善与图像恢复的初步正向证据。

### 2. 本轮目的

本轮不修改固件，只把 5S 的每块单次读取扩大为连续统计。默认对 block 0 和 block 2048 各执行 20 次 `SD ATK1BREAD`，记录每次 HAL 状态、错误分类、卡状态、耗时和完整 512B buffer 指纹，验证两块是否均达到 20/20、100% PASS。

### 3. 自动测试脚本

新增：

`tools/uart_sd_dvp_mask_repeat_test.py`

脚本按以下顺序驱动已有 CLI：

```text
SD INIT
SNAPSHOT PREPARE
SD DVPSTATUS
SD DVPSTOP
SD DVPSTATUS
SD TAKEOVER ENTER
SD ATK1BINIT
SD ATK1BSTATUS
循环：SD ATK1BREAD <block> + SD ATK1BSTATUS
SD TAKEOVER EXIT
SD DVPRESTORE
SD DVPSTATUS
SNAPSHOT RESTORE
STATUS
```

若 DVPSTOP、TAKEOVER ENTER 或 ATK1BINIT 任一步失败，读循环标记为 SKIP，但脚本仍尽量执行 TAKEOVER EXIT、DVPRESTORE、SNAPSHOT RESTORE 和 STATUS。DVPRESTORE 以实际保存的 `0x3018` 原值作为恢复校验目标，不硬编码必须恢复为 `0xFF`。

脚本生成 `captures/sd_dvp_mask_repeat_<timestamp>.csv`、对应 `_log.txt` 和 `_summary.txt`。CSV 保存逐次读结果与 buffer 指纹；summary 保存 DVP mask/restore、takeover、ATK1B init、每个 block 的成功率和指纹唯一值统计以及清理结果。

### 4. 运行命令

```text
python tools/uart_sd_dvp_mask_repeat_test.py --port COM4 --baud 115200 --read-count 20 --blocks 0,2048
```

默认参数为 COM4、115200、每块 20 次、blocks 0/2048、命令超时 5.0 秒和 tag `dvp_mask_repeat`。

### 5. 测试后图像恢复验证

自动脚本不发送二进制图像请求。脚本完成并确认清理结果后，仍必须手动运行：

```text
python tools/uart_image_request_basic.py
python tools/uart_image_request_repeat.py
```

需要同时检查 basic、repeat 20/20、frame_id 连续性和系统错误计数。

### 6. 结果判断

- 若 block 0 与 block 2048 均为 20/20 PASS，且后续图像 basic/repeat PASS，则 C1 可进入下一阶段主流程集成设计。
- 若 DVPSTOP 成功但连续读仍出现 `DATA_CRC_FAIL`、NOT_READY 或其他错误，说明只释放 D2/D3/D4 的 C1 方案仍不足，不能直接集成。
- 若连续读达到 100% PASS 但 DVPRESTORE 或图像恢复失败，说明 C1 的恢复风险不可接受，不能并入主流程。
- 本轮不执行硬件测试、不修改固件、不接 FATFS、不写卡、不启用 SDIO DMA/IRQ，也不提交 Git commit。

## Stage 11C-5U DVP mask + SD takeover + 图像恢复多轮稳定性测试

### 1. Stage 11C-5T 结果

- `SD DVPSTOP` 将 OV5640 `0x3018` 从 `0xFF` 改为 `0x8F`，回读为 `0x8F`。
- block 0 连续读取 20/20 PASS，PASS_RATE=100.00%。
- block 2048 连续读取 20/20 PASS，PASS_RATE=100.00%。
- `SD DVPRESTORE` 将 `0x3018` 从 `0x8F` 恢复到保存的 `0xFF`，回读为 `0xFF`。
- basic 图像恢复 PASS，repeat 图像恢复 20/20 PASS，frame_id 连续。

因此，OV5640 `0x3018[6:4]` DVP pad mask 已证明能够释放 PC8、PC9、PC11 共享线，使 SDIO 1-bit polling read 稳定通过；恢复原值后相机图像链路也能够恢复。

### 2. 本轮目的

本轮把 5T 的单次完整流程扩展为默认 10 cycles 的多轮稳定性测试。每轮都执行 DVP mask、SD takeover、ATK1B block 0/2048 读取、DVP restore，并在恢复 snapshot 后发送一次 binary image request，同时验证 SD 读块和图像恢复。

某轮失败时脚本不终止后续轮次；下一轮开始前会尽量执行 `SD TAKEOVER EXIT`、`SD DVPRESTORE`、`SNAPSHOT RESTORE` 和 `STATUS` 清理。脚本记录失败轮次以及各环节累计成功/失败次数。

### 3. 自动测试脚本

新增：

`tools/uart_sd_dvp_mask_cycle_stability.py`

每轮按以下顺序执行：

```text
SD INIT
SNAPSHOT PREPARE
SD DVPSTOP
SD DVPSTATUS
SD TAKEOVER ENTER
SD ATK1BINIT
SD ATK1BREAD 0
SD ATK1BREAD 2048
SD TAKEOVER EXIT
SD DVPRESTORE
SD DVPSTATUS
SNAPSHOT RESTORE
STATUS
OV56RGB5 binary image request
```

binary request 沿用现有 14B 请求格式，每轮递增 seq；响应必须为 38426B、magic 为 `OV56RGB5`、payload CRC32 正确且 frame_id 可解析，才记为 IMAGE_PASS。脚本不保存 PNG，只生成 `captures/sd_dvp_mask_cycle_<timestamp>.csv`、对应 `_log.txt` 和 `_summary.txt`。

### 4. 运行命令

```text
python tools/uart_sd_dvp_mask_cycle_stability.py --port COM4 --baud 115200 --cycles 10 --blocks 0,2048
```

默认参数为 COM4、115200、10 cycles、blocks 0/2048、CLI 超时 5.0 秒、图像超时 10.0 秒和 tag `dvp_mask_cycle`。

### 5. 结果判断

- 若 10/10 cycles PASS，则 C1 可进入主流程集成设计。
- 若 SD 读失败，说明 DVP mask 仍有边界问题，不能直接集成。
- 若图像恢复失败，说明 DVP restore 或整体 restore flow 仍有风险。
- 若出现 IWDG skip、UART DMA error、hook fault 或 stream overflow，必须先修复稳定性问题。

单轮 PASS 要求 snapshot prepare、DVPSTOP、`0x3018` mask 回读、takeover enter、ATK1B init、block 0/2048 读取、takeover exit、DVPRESTORE、`0x3018` 原值恢复、snapshot restore、系统状态和 binary image request 全部通过，且 IWDG/UART/HOOK/stream overflow 计数均为 0。

本轮只新增主机端自动测试脚本并更新文档，不修改固件；Codex 不执行硬件测试，也不提交 Git commit。

## Stage 11C-5U-1 DVP mask 多轮脚本失败定位

### 1. 现象与结论修正

Stage 11C-5T 的单轮连续读块验证已经成功：DVPSTOP 将 OV5640 `0x3018` 从 `0xFF` 改为 `0x8F`，TAKEOVER ENTER 和 ATK1B INIT 均通过，block 0 与 block 2048 各连续读取 20/20 PASS；DVPRESTORE 将 `0x3018` 从 `0x8F` 恢复到 `0xFF`，basic/repeat 图像恢复也全部 PASS。

Stage 11C-5U 首次多轮测试出现以下异常：

- DVPSTOP 10/10 PASS。
- TAKEOVER ENTER 10/10 PASS。
- ATK1BINIT 0/10 PASS，读块因此全部跳过。
- DVPRESTORE 与 SNAPSHOT RESTORE 10/10 PASS。
- IMAGE 6/10 PASS，frame_id 不连续。
- IWDG、HOOK、UART DMA 与 stream overflow 计数均为 0。

失败集中在 5U 自动循环脚本的 ATK1BINIT 阶段。由于 5T 已经证明相同 DVP mask 下 ATK1B 连续读块稳定，当前不能据此判定 C1 方案失败；需要先用更完整的逐轮状态确认 HAL init、CardInfo 或 card transfer wait 的具体失败点。

### 2. 本轮脚本增强

本轮只增强 `tools/uart_sd_dvp_mask_cycle_stability.py`，不修改固件：

- 新增 ATK1BINIT 详细字段，记录 init ready、HAL init status/error、CardInfo status/error、transfer wait 计数、最后卡状态、耗时、ClockDiv 和 BusWidth。
- 新增 DVPSTOP、TAKEOVER ENTER、DVPRESTORE 的 active、寄存器、错误码、错误文本和关键前置状态字段。
- 新增 `--cycle-gap 1.0`、`--restore-wait 1.0`、`--image-wait 1.0` 与 `--cleanup-wait 0.5`。
- 每轮正式流程前无条件执行 `SD TAKEOVER EXIT`、`SD DVPRESTORE`、`SNAPSHOT RESTORE`、`STATUS` 防御性清理；清理命令之间按 cleanup-wait 等待，清理结果只记录为 pre_cleanup，不参与本轮 PASS/FAIL。
- SNAPSHOT RESTORE 后先等待 restore-wait，STATUS 后再等待 image-wait，然后发送递增 seq 的 binary image request。
- ATK1BINIT 失败时只跳过 block read，仍继续执行 TAKEOVER EXIT、DVPRESTORE、DVPSTATUS、SNAPSHOT RESTORE、STATUS 和 image request，并记录 `ATK1B_INIT_FAIL`。

### 3. 输出增强

CSV 与逐轮日志记录完整 ATK1B、DVP、takeover、restore 和 image 诊断字段。image 失败时额外记录接收长度、错误文本、是否找到 `OV56RGB5` magic、frame_id 和 CRC 结果。

summary 保留 5U 原字段，并新增以下失败定位统计：

```text
ATK1B_HAL_ERROR_VALUES
ATK1B_HAL_STATUS_VALUES
ATK1B_CARDINFO_STATUS_VALUES
ATK1B_LAST_CARD_STATE_VALUES
ATK1B_INIT_READY_VALUES
ATK1B_FAILED_CYCLES
DVP_MASK_AFTER_VALUES
DVP_MASK_SAVED_VALUES
DVP_MASK_FAILED_CYCLES
TAKEOVER_ERROR_CODE_VALUES
TAKEOVER_ERROR_TEXT_VALUES
SDIO_FULL_GPIO_AF12_SELECTED_VALUES
DVP_RESTORE_AFTER_VALUES
DVP_RESTORE_FAILED_CYCLES
IMAGE_FAILED_CYCLES
IMAGE_RX_LEN_VALUES
IMAGE_ERROR_VALUES
FAIL_REASON_COUNTS
```

### 4. 下一步判断

下一次开发板测试应首先查看 `ATK1B_HAL_ERROR_VALUES`、`ATK1B_HAL_STATUS_VALUES`、CardInfo 状态、最后卡状态和失败轮次，再判断问题属于 HAL_SD_Init、CardInfo、transfer wait 还是轮间状态残留。在取得这些证据前不修改固件，也不否定 C1 DVP mask 方案。

本轮 Codex 不执行硬件测试，不提交 Git commit。

## Stage 12A CLI / STATUS 删减清理

### 1. 删减原因

Stage 11C 已经完成 SDIO 与 OV5640 共享线的诊断闭环。继续保留大量实验命令、寄存器快照、缓冲区指纹和过程计数器会污染最终工程；原 STATUS 与 SD STATUS 输出也过长，不适合正常项目展示。因此本阶段直接删除历史诊断 CLI、相关解析分支和冗余状态字段，不通过宏或注释保留旧实现。

### 2. 最终保留 CLI

- `HELP`
- `STATUS`
- `PROC [BYPASS|GRAY|BINARY]`
- `THR [0..255]`
- `RESET`
- `DUMP`
- `SD STATUS`

### 3. 删除的 Stage 11C 诊断 CLI

- `SD CARDINFO`
- `SD READTEST`
- `SD READINFO`
- `SD BUSWIDTH`
- `SD LINESTATE`
- `SD ATKINIT`
- `SD ATKSTATUS`
- `SD ATK1BINIT`
- `SD ATK1BREAD`
- `SD ATK1BSTATUS`
- `SD SENSORSTOP`
- `SD SENSORRESTORE`
- `SD SENSORSTATUS`
- `SD DVPSTOP`
- `SD DVPRESTORE`
- `SD DVPSTATUS`
- `SD INIT`
- `SD TAKEOVER STATUS`
- `SD TAKEOVER ENTER`
- `SD TAKEOVER EXIT`
- `SNAPSHOT STATUS`
- `SNAPSHOT PREPARE`
- `SNAPSHOT RESTORE`
- `IWDGTEST CAMERA_TIMEOUT`

### 4. STATUS 保留内容

STATUS 仅保留模式与配置、核心健康、关键故障和看门狗关键状态：

- `STATUS`: `mode`、`threshold`、`frame`、`tuning`、`uptime_ms`
- `HEALTH`: `heap_free`、`heap_min`、`stack_camera_min`、`stack_monitor_min`
- `FAULT`: `hook_fault`、`assert_line`、`uart_dma_error`、`stream_overflow`
- `IWDG`: `enabled`、`refresh_skip`、`last_skip_reason`

STATUS 不再输出 CLI、DUMP、UART 空闲、二进制请求分类、心跳年龄、IWDG 刷新时间等细碎计数器。Stage 12A 后续回归验证表明这些 RTOS 内部字段不能随输出一起删除，因此内部统计与 DUMP/binary request/UART DMA/stream buffer 运行链路保持原状。

### 5. SD STATUS 保留内容

SD STATUS 仅保留：

- `supported`
- `card_ready`
- `takeover_required`
- `sdio_ready`
- `fatfs_ready`
- `last_error`
- `dvp_mask_solution=OV5640_3018_6_4`

SD 状态结构只保留 SD 支持/ready、SDIO/FATFS ready、takeover 需求、最后错误、DVP mask 核心状态、保存的 `0x3018`、恢复状态、最近初始化结果和最近读写结果。ATK、READTEST、LINESTATE、BUSWIDTH、GPIO 完整 readback、SDIO 寄存器快照、缓冲区指纹及实验计数器均已删除。

### 6. 保留的内部流程能力

OV5640 `0x3018[6:4]` DVP mask 与原值恢复继续保留，供后续 SD snapshot 内部流程使用；SDIO GPIO takeover/restore 和基础 HAL SD 初始化能力也继续保留。串口不再暴露 `DVPSTOP`/`DVPRESTORE`，也不再保留 ATK、READTEST、LINESTATE 等实验命令。本阶段不接 FATFS、不写卡、不启用 SDIO DMA 或 IRQ。

## Stage 12B SD 诊断残留代码删减

### 1. RTOS 回归边界

Stage 12A 曾把 STATUS 输出删减错误地扩大到 `camera_rtos.c/.h` 内部状态删减，导致文本 DUMP 无响应、binary image request 失败。恢复 RTOS 内部统计及请求链路后，DUMP、basic 和 repeat 图像请求恢复通过。因此 Stage 12B 明确冻结 `camera_rtos.c/.h`：STATUS 可以保持简洁，但 DUMP、binary request、UART DMA、stream buffer 与内部统计字段不再删改。

### 2. 已清理的 SD 诊断残留

SD 模块不再包含 ATK official 4-bit、ATK1B、READTEST/READINFO、LINESTATE、BUSWIDTH、SENSORSTOP/SENSORRESTORE/SENSORSTATUS 等实验入口或状态输出代码。读块 buffer 指纹、SDIO register snapshot、GPIO 逐 pin readback，以及 attempt/success/error/operation_ms 细碎计数器均不保留。

### 3. SD 最小状态

`CameraSdStorageStatus_t` 仅保留：

- `supported`
- `card_ready`
- `takeover_required`
- `sdio_ready`
- `fatfs_ready`
- `last_error_code` / `last_error_text`
- `dvp_mask_available` / `dvp_mask_active`
- `dvp_reg_3018_saved` / `dvp_reg_3018_current_or_restored`
- `last_sd_init_status` / `last_sd_init_error`
- `last_sd_rw_status` / `last_sd_rw_error`

当前尚未接入 SD 读写主流程，`last_sd_rw_status/error` 只保留最小未运行占位，不引入诊断逻辑。

### 4. 保留的内部主流程能力

- 保存 OV5640 `0x3018`，写入 `saved_3018 & 0x8F`，并恢复保存原值。
- 将 PC8/PC9/PC10/PC11/PC12/PD2 切换到 SDIO AF12；退出后把 PC8/PC9/PC11 恢复为 DCMI AF13，并把 PC10/PC12/PD2 退回安全输入态。
- 保留 SDIO clock enable/disable、`HAL_SD_Init`、`HAL_SD_DeInit` 和必要的 card ready/error 状态。
- SD STATUS 仍只输出 `supported`、`card_ready`、`takeover_required`、`sdio_ready`、`fatfs_ready`、`last_error` 和 `dvp_mask_solution=OV5640_3018_6_4`。

Stage 12B 不接 FATFS、不写卡、不启用 SDIO DMA/IRQ，不执行硬件测试，也不提交 Git commit。

## Stage 12C camera_rtos 引用审计与安全瘦身

### 1. 回归边界

Stage 12A 已证明：不能因为字段不再由 STATUS 输出，就直接删除 DUMP、binary request、UART DMA、stream buffer、IWDG 或 heartbeat 链路依赖的内部状态。本轮先逐字段、逐函数搜索读写者和调用者，只删除已确认不参与输出、判断或主流程的代码；图像请求分发、采集、发送及 UART 恢复流程均未改动。

### 2. 已删除的无功能依赖字段

- 只写不读的任务循环统计：`camera_service_loop_count`、`monitor_tick_count`
- 已无 CLI 调用者的统计：`cli_command_count`、`cli_unknown_count`
- 与真实 `UartRxDmaStats_t::uart_error_count` 重复且不再被读取的 RTOS 镜像：`CameraRtosStats_t::uart_error_count`
- 已无 STATUS 调用者的时间记录：`last_status_time_ms`
- 只写不读的健康采样次数：`health_sample_count`
- 已不再输出且不参与 IWDG 判断的刷新展示字段：`iwdg_refresh_count`、`iwdg_last_refresh_ms`、`iwdg_last_skip_ms`
- 仅复制编译期常量且不参与判断的字段：`iwdg_timeout_ms`、`iwdg_camera_age_limit_ms`、`iwdg_monitor_age_limit_ms`
- 只服务于已删除 `IWDGTEST CAMERA_TIMEOUT` CLI 的字段：`iwdg_test_mode`

IWDG 心跳年龄阈值宏仍由正式刷新判断直接使用；删除的只是未被读取的结构体镜像。

### 3. 已删除的无调用者函数

- `Camera_RTOS_EnableIwdgCameraTimeoutTest`
- `Camera_RTOS_RecordCliCommand`
- `Camera_RTOS_RecordCliUnknown`
- `Camera_RTOS_RecordUartError`
- `Camera_RTOS_RecordStatus`
- static `Camera_RTOS_SyncUartErrorCount`

其中 UART DMA 的实际错误计数、恢复函数和 STATUS 读取均继续使用 `uart_rx_dma` 模块，不受 RTOS 重复镜像删除影响。

### 4. 保留与暂缓删除

- 保留当前 STATUS 所需的 `uptime_ms`、stack/heap、`hook_fault_code`、`assert_line`、`iwdg_enabled`、`iwdg_refresh_skip_count`、`iwdg_last_skip_reason`。
- 保留 Hook 记录接口及 `hook_fault_count`；该字段属于明确冻结的 Hook fault/assert 记录范围。
- 保留全部 heartbeat count/time/age 字段；它们参与 IWDG 正式刷新判断。
- 保留真实 UART DMA/stream buffer 的 event、byte、overflow、error、recovery、resync 状态及处理逻辑。
- 暂缓删除 DUMP request/success/error、`last_dump_time_ms`、`last_error_code` 及其记录接口，避免再次触碰已经发生过回归的 DUMP 链路。
- 暂缓删除 binary request 成功/错误分类、timeout、last sequence/error 字段及记录函数，避免触碰 binary request 解析与响应链路。
- 暂缓删除 `uart_none_count`、`uart_pending_count` 及其记录接口；它们位于当前 UART 主循环路径，留待有硬件回归条件时另行处理。

### 5. 范围约束

Stage 12C 不修改 Core、CMake、tools、SD storage、PC dump、UART dispatcher、image request、CRC、OV5640、SCCB 或 LCD 模块；不新增 CLI，不接 FATFS，不写卡，不启用 SDIO DMA/IRQ，不执行硬件测试，也不提交 Git commit。

## Stage 12D 最终 SD snapshot 内部流程接口设计

### 1. 当前阶段结论

- SDIO 硬件本身正常。
- SD 卡、卡座、`HAL_SD_Init` 和 `HAL_SD_ReadBlocks` polling 路径已经通过 SD-only 与 DVP mask 诊断验证。
- 正常工程中的 SDIO 读块失败并非 SDIO、SD 卡或卡座故障，根因是 OV5640 DVP 输出持续驱动共享的 PC8/PC9/PC11。
- 已验证的有效方案是：进入 SD 会话时保存 OV5640 `0x3018` 原值，并将 `0x3018[6:4]` 清零；退出 SD 会话时写回保存值，使 DVP 图像链路恢复。
- Stage 12 已删除大量 Stage 11C 诊断 CLI。最终工程不再通过串口暴露分步骤 takeover、DVP mask、读块或传感器控制等调试入口，SD snapshot 必须通过完整的内部函数链路执行。

### 2. 最终 CLI 策略

当前最终 CLI 只保留以下 7 个命令：

```text
HELP
STATUS
PROC [BYPASS|GRAY|BINARY]
THR [0..255]
RESET
DUMP
SD STATUS
```

- 不恢复 `SD ATK*`。
- 不恢复 `SD DVPSTOP`、`SD DVPRESTORE`、`SD DVPSTATUS`。
- 不恢复 `SD READTEST`、`SD READINFO`。
- 不恢复 `SNAPSHOT PREPARE`、`SNAPSHOT RESTORE`。
- 后续真正保存图像时，可以增加一个最终用户功能命令，例如 `SD SNAPSHOT`；该命令必须一次触发完整的 begin/save/end 流程，不得把内部步骤重新拆成临时诊断命令。

### 3. 最终 SD snapshot 内部流程

本节只定义接口职责与执行顺序，Stage 12D 不实现代码。

#### `Camera_SDStorage_SnapshotBegin()`

1. 获取 snapshot software guard，拒绝并发 DUMP、binary image request 或另一个 SD snapshot；停止 DCMI，暂停相机输出链路。
2. 读取并保存 OV5640 `0x3018` 原值，同时记录“原值已保存”状态，供任意失败路径恢复。
3. 写入 `saved_3018 & 0x8F`，释放 OV5640 D2/D3/D4，即清除 `0x3018[6:4]`。
4. 将 PC8/PC9/PC10/PC11/PC12/PD2 切换为 SDIO AF12。
5. 使能 SDIO clock。
6. 执行 `HAL_SD_Init`，成功后进入 `CARD_READY`。
7. Stage 13A 在此基础上接入 FATFS `f_mount`，成功后进入 `FATFS_READY`。

`SnapshotBegin()` 只有在上述步骤全部成功后才返回成功；任一步失败均进入统一清理路径，不允许调用方自行拼接恢复步骤。

#### `Camera_SDStorage_SnapshotSave()`

1. Stage 13C 才实现，本阶段仅保留接口设计。
2. 仅允许在 `FATFS_READY` 状态调用，并从当前 front frame buffer 获取一帧稳定图像。
3. 创建并写入 SD 卡文件，更新保存结果、文件名和文件大小等最终状态。
4. 文件写入完成后关闭文件；成功或失败均转入统一 `SnapshotEnd()`/cleanup 流程。

#### `Camera_SDStorage_SnapshotEnd()`

1. 如果文件已打开，执行 FATFS close；如果文件系统已挂载，执行 unmount。相关能力在后续 Stage 13 实现。
2. 如果 SD HAL 已初始化，执行 `HAL_SD_DeInit`。
3. 关闭 SDIO clock。
4. 将 PC8/PC9/PC11 恢复为 DCMI AF13。
5. 将 PC10/PC12/PD2 恢复为安全输入状态。
6. 如果 OV5640 `0x3018` 原值已保存，写回保存值。
7. 解除 snapshot software guard，随后恢复相机链路；内部状态保持 `CLEANUP`，只有恢复完成并回到 `IDLE` 后才重新接受 DUMP、binary image request 或下一次 snapshot。

`SnapshotEnd()` 必须是幂等的尽力清理接口：每个步骤依据已完成标志决定是否执行，即使前一步失败也继续尝试后续恢复。

### 4. 推荐内部状态机

| 状态 | 含义 | 可进入条件 | 退出条件 |
|---|---|---|---|
| `IDLE` | 普通图像采集状态 | 上电初始化完成或清理恢复完成 | 请求 SD snapshot 并成功获取 guard |
| `CAMERA_PAUSED` | DCMI 已暂停 | `SnapshotBegin` 第一步成功 | 成功保存并 mask DVP，或失败转清理 |
| `DVP_MASKED` | OV5640 D2/D3/D4 已释放 | `0x3018` 保存和写入成功 | SDIO GPIO takeover 成功，或失败转清理 |
| `SDIO_ACTIVE` | STM32 引脚已切到 SDIO | GPIO AF12 切换与 SDIO clock 使能成功 | SD init 成功，或失败转清理 |
| `CARD_READY` | SD HAL 初始化成功 | `HAL_SD_Init` 成功 | FATFS mount 成功，或失败转清理 |
| `FATFS_READY` | 文件系统可用 | `f_mount` 成功 | 开始保存并成功打开文件，或请求正常结束 |
| `SAVING` | 正在写文件 | 文件打开成功 | 写入完成或失败后转清理 |
| `CLEANUP` | 正在按统一顺序清理和恢复 | 保存成功、主动结束或任一步失败 | 所有可执行恢复步骤完成 |
| `ERROR` | 本次流程发生错误 | 任一步失败并记录首个错误 | 强制进入 `CLEANUP`，完成后回到 `IDLE` |

状态迁移由 SD storage 模块内部控制，外部调用方不能直接改写状态，也不能跳过 `SnapshotBegin()` 或 `SnapshotEnd()`。

### 5. 错误清理顺序

无论在哪一步失败，都保留首个业务错误，并按以下顺序尽力清理；清理步骤本身失败时记录清理错误，但不得中断剩余恢复动作：

1. 如果文件已打开，关闭文件。
2. 如果 FATFS 已挂载，卸载或标记为未挂载。
3. 如果 SD HAL 已初始化，执行 `HAL_SD_DeInit`。
4. 关闭 SDIO clock。
5. 恢复 SDIO GPIO / DCMI GPIO：PC8/PC9/PC11 回到 DCMI AF13，PC10/PC12/PD2 回到安全输入状态。
6. 如果 OV5640 `0x3018` 已保存，写回原值。
7. 解除 snapshot software guard。
8. 恢复图像链路；此时内部状态仍为 `CLEANUP`，不得接受新请求进入半恢复状态。
9. 更新 SD STATUS `last_error`，并使内部状态最终回到 `IDLE`。

### 6. 必须保留的内部能力

以下能力是最终 SD snapshot 主流程依赖，不得作为旧诊断残留删除：

- OV5640 `0x3018[6:4]` mask 与保存原值恢复能力。
- SDIO GPIO takeover / restore。
- SDIO clock enable / disable。
- `HAL_SD_Init` / `HAL_SD_DeInit`。
- 后续 FATFS mount / open / write / close。
- front frame buffer 稳定帧读取能力。
- DUMP / binary image request 链路及其互斥保护。
- FreeRTOS / UART DMA / stream buffer / IWDG 健康链路。

### 7. SD STATUS 最终设计

当前阶段继续保持简洁输出：

```text
SD STATUS:
  supported=
  card_ready=
  takeover_required=
  sdio_ready=
  fatfs_ready=
  last_error=
  dvp_mask_solution=OV5640_3018_6_4
```

Stage 13 可随最终保存功能增加：

```text
last_snapshot_result=
last_file=
last_file_size=
save_count=
save_error=
```

这些字段只描述最终功能结果，不得恢复 Stage 11C 的 ATK、READTEST、LINESTATE、GPIO/SDIO 寄存器或逐步骤诊断输出。

### 8. 后续阶段规划

#### Stage 13A：FATFS 最小挂载

- 只实现 `f_mount` 和必要的卸载/错误清理。
- 不创建或写入文件。
- `SD STATUS` 显示 `fatfs_ready`。

#### Stage 13B：SD 文件写入固定字符串

- 创建固定测试文件。
- 写入固定字符串并正确 close。
- 通过读回或电脑检查验证内容。
- 仍不保存图像。

#### Stage 13C：保存一帧 RGB565 图像

- 从 front frame buffer 获取 `160x120 RGB565` 图像。
- 优先保存为 raw；BMP 作为后续增强，不与首次图像保存同时引入。
- 保存完成后执行统一 cleanup，确保相机链路恢复。

#### Stage 13D：最终 `SD SNAPSHOT` 命令

- 只增加一个最终功能命令，一次触发完整 snapshot 流程。
- 不恢复或新增分步骤诊断命令。

### 9. 禁止事项

- 不恢复 `SD ATK*`。
- 不恢复 `SD DVP*`。
- 不恢复 `SD SENSOR*`。
- 不恢复 `SD READTEST` / `SD READINFO`。
- 不恢复 `SD LINESTATE` / `SD BUSWIDTH`。
- 不恢复 `SNAPSHOT PREPARE` / `SNAPSHOT RESTORE`。
- 不新增临时 CLI。
- 不启用 SDIO DMA。
- 不启用 SDIO IRQ。
- 不触碰 PWDN / `CAMOFF` / `CAMON`。
- 不写 OV5640 `0x3008`。
- 不写 OV5640 `0x4202`。

## Stage 13A FATFS 最小挂载检查

### 1. 本轮目的

- 计划复用现有 `SD STATUS`，由该命令触发一次完整的内部 SD + FATFS mount 检查。
- 不新增临时 CLI，不恢复 Stage 11C 的分步骤诊断命令。
- 本阶段只检查 `f_mount`，不写卡、不创建文件、不保存图像。

### 2. FatFs 源码扫描与阶段结论

已扫描当前工程中的以下文件和目录：

- `ff.c`
- `ff.h`
- `ffconf.h`
- `diskio.c`
- `diskio.h`
- `ffsystem.c`
- `ffunicode.c`
- `FATFS/`
- `Middlewares/Third_Party/FatFs/`

扫描结果：当前仓库没有 FatFs 源码、头文件、配置文件或 disk I/O 适配层。工程中现有的 `fatfs_ready` 字段和相关注释只是状态占位，不是可调用的 FatFs 实现。

根据 Stage 13A 的来源约束，本阶段暂停代码实现：不从网络下载、不手写简化版 `ff.c`、不复制未知来源代码、不修改 `CMakeLists.txt`，也不执行构建。待项目提供可确认来源且许可证明确的完整 FatFs 源码后，再继续接入。

### 3. 源码到位后的内部流程

FatFs 源码到位后，`Camera_SDStorage_CheckFatfsMount()` 应一次性执行以下安全流程，外部不得通过临时 CLI 分步骤调用：

1. pause camera，获取 software guard 并暂停 DCMI/相机输出链路。
2. 保存 OV5640 `0x3018` 原值，写入 `saved_3018 & 0x8F` 完成 DVP mask。
3. takeover SDIO GPIO，使能 SDIO clock。
4. 执行 `HAL_SD_Init`。
5. 执行只读优先的 `f_mount` 检查，并记录 mount 结果。
6. 如果挂载成功，执行 `f_mount(NULL)` 卸载；不调用 `f_open`、`f_write` 或 `f_read`。
7. 执行 `HAL_SD_DeInit`，关闭 SDIO clock。
8. 恢复 SDIO/DCMI GPIO，写回 OV5640 `0x3018` 保存值。
9. 解除 software guard，恢复图像链路。
10. 更新 `last_mount` 和 `last_error`。

无论在哪一步失败，都必须继续尽力执行 unmount、SD HAL deinit、clock disable、GPIO restore、`0x3018` restore、guard release 和图像链路恢复。不得调用 `HAL_SD_WriteBlocks`，不得启用 SDIO DMA/IRQ。

### 4. 判断标准

完成后应满足：

- `SD STATUS` 显示 `last_mount=PASS`。
- `SD STATUS` 显示 `last_error=OK`。
- 检查结束后 DUMP 正常。
- `tools/uart_image_request_basic.py` PASS。
- `tools/uart_image_request_repeat.py` PASS。

由于当前缺少 FatFs 源码，本轮未实现挂载检查，以上标准尚未执行。现有 `SD STATUS`、HELP 7 命令、DVP mask 内部能力和错误清理基础流程均保持原状。

## Stage 13A-0 FatFs 源码补齐与接入方案确认

### 1. 当前问题

- 当前 `ISP_OV5640` 工程目录内没有 FatFs 源码、头文件、配置文件或 disk I/O 适配层。
- 因缺少 `ff.c`、`ff.h`、`ffconf.h` 和 `diskio.h`，Stage 13A 当前无法实现或验证 `f_mount`。
- 当前正式分支已经删除 `SD READTEST`、`SD ATK1BREAD` 等 Stage 11C 诊断 CLI，因此不能依赖旧串口诊断命令直接测试 SD 卡文件系统；后续仍应复用简洁的 `SD STATUS` 触发完整内部检查。

### 2. FatFs 源码查找结果

| 搜索路径 | 是否找到 | 关键文件 |
|---|---|---|
| `D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ISP_OV5640` | 否 | 未找到 FatFs 文件或目录 |
| `D:\MCU+FreeRTOS\STM32_HAL\ISP_Project` | 是 | `ff14b\source\ff.c`、`ff.h`、`ffconf.h`、`diskio.c`、`diskio.h`、`ffsystem.c`、`ffunicode.c` |
| `D:\MCU+FreeRTOS\STM32_HAL` 下的其他工程 | 无额外结果 | 仅找到上述 `ff14b\source` 候选包 |
| `C:\Users\FAKE\STM32Cube\Repository` | 否 | 未找到 `STM32Cube_FW_F4_*` 固件包或 FatFs 源码 |

找到的候选源码位于：

```text
D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ff14b\source
```

文件头确认该候选包是 ChaN FatFs R0.14b（2021），上级目录包含 `LICENSE.txt`。它是本地独立 FatFs 发布包，不是 STM32CubeF4 固件包。R0.14b 的 `ff.h` 已直接定义所需整数类型，该包没有 `integer.h`，因此不能把旧版本的 `integer.h` 作为强制依赖混入。

### 3. 推荐接入文件

待用户确认采用本地 R0.14b 及其许可证后，建议把第三方源码按原始版权声明纳入项目的独立 vendor 目录，例如 `Middlewares/Third_Party/FatFs/src/`：

- 必需：`ff.c`
- 必需：`ff.h`
- 必需并按项目审查配置：`ffconf.h`
- 必需：`diskio.h`
- 建议随包保留：`LICENSE.txt`
- 条件文件：`ffsystem.c`
- 条件文件：`ffunicode.c`

不建议复制候选包的 `diskio.c` 作为项目实现：该文件头明确标注为 generic skeleton，内部引用未实现的 RAM/MMC/USB 示例函数，不能直接编译为本项目 SDIO 适配层。

当前候选 `ffconf.h` 的关键配置为 `FF_FS_READONLY=0`、`FF_USE_LFN=0`、`FF_FS_REENTRANT=0`、`FF_FS_EXFAT=0`、扇区大小固定 512 字节。Stage 13A 接入时应优先将 `FF_FS_READONLY` 配置为 `1`，并继续禁用 LFN、exFAT 和 FatFs 内部重入支持，以保持最小只读挂载范围。

`ffsystem.c` 仅在 `FF_USE_LFN == 3` 或 `FF_FS_REENTRANT != 0` 时提供有效实现；`ffunicode.c` 仅在 `FF_USE_LFN != 0` 时需要。以 Stage 13A 的 `FF_USE_LFN=0`、`FF_FS_REENTRANT=0` 配置，两者不需要加入构建，但是否纳入必须以最终 `ffconf.h` 为准。

### 4. `diskio.c` 接入策略

- 不直接复用本地候选包或其他未知工程中的 `diskio.c`。
- 本项目应新增一个最小、可审查的 disk I/O 适配层，只把 FatFs 标准接口映射到已有 SD storage 和 SDIO 安全会话能力。
- `Camera_SDStorage_CheckFatfsMount()` 负责完整会话生命周期：camera pause、DVP mask、SDIO takeover、`HAL_SD_Init`、`f_mount`、unmount、deinit 和 restore。
- disk I/O 适配层的 `disk_initialize`、`disk_read` 和 `disk_ioctl` 只调用本项目受控的 SD storage API，并且只允许在上述安全会话已经激活时访问 SDIO；不得在每次扇区读取时重复切换 DVP/SDIO GPIO。
- Stage 13A 只实现 `disk_status`、`disk_initialize`、`disk_read`、`disk_ioctl`。`disk_read` 底层使用已经验证的 `HAL_SD_ReadBlocks` polling 路径。
- Stage 13A 的只读配置不实现实际写入；如果接口兼容需要保留 `disk_write` 符号，只能返回 `RES_WRPRT`，不得调用 `HAL_SD_WriteBlocks`。Stage 13B 经单独审查后再启用写入。

### 5. Stage 13A 实现边界

- 只执行立即挂载检查，例如 `f_mount(..., opt=1)`，并在检查后卸载。
- 不调用 `f_open`。
- 不调用 `f_write`。
- 不调用 `f_read`。
- 不调用 `HAL_SD_WriteBlocks`。
- 不新增临时 CLI，不恢复 Stage 11C 诊断命令。
- 仍复用 `SD STATUS` 触发最小挂载检查，并输出简洁的 `last_mount`、`last_error` 结果。
- 不启用 SDIO DMA 或 SDIO IRQ，继续使用 polling 读路径。

### 6. 源码缺失时的处理

如果最终不采用本地 R0.14b 候选包，且仍未找到官方 STM32CubeF4 FatFs：

- 不继续实现 Stage 13A。
- 先安装官方 STM32CubeF4 固件包，或使用 CubeMX 生成一个带 FatFs 的 STM32F4 参考工程，再审查并把对应 FatFs 源码纳入当前工程。
- 不从非官方来源、来源不明的旧工程或网络片段复制实现。
- 未获得完整、版本一致的 `ff.c`、`ff.h`、`ffconf.h`、`diskio.h` 和许可证前，不修改 CMake，也不编写替代版 FatFs。

Stage 13A-0 只完成本地查找与方案确认：本轮不复制文件、不修改代码、不接入 CMake、不构建、不执行硬件测试，也不提交 Git commit。

## Stage 13A-1 FatFs R0.14b 接入与最小 f_mount 检查

### 1. FatFs 来源与复制文件

FatFs 来源：

```text
D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ff14b
```

已复制到 `Middlewares/Third_Party/FatFs/src/`：

- `ff.c`
- `ff.h`
- `ffconf.h`
- `diskio.h`
- `LICENSE.txt`

没有复制 `ffsystem.c` 或 `ffunicode.c`：当前 `FF_USE_LFN=0`、`FF_FS_REENTRANT=0`，R0.14b 在该配置下不需要这两个条件源码。没有复制 generic `diskio.c`。

### 2. 不使用 generic `diskio.c` 的原因

R0.14b 自带的 `diskio.c` 是 generic skeleton，引用未实现的 RAM/MMC/USB 示例函数，不包含本项目的 OV5640 DVP mask、SDIO GPIO takeover、HAL SD 句柄或错误清理约束。直接复制既不能形成可用块设备，也可能绕过共享引脚保护，因此本项目使用独立的 `camera_fatfs_diskio.c/.h`。

### 3. 本项目 diskio 适配策略

- FatFs 物理驱动号固定为 0。
- 实现 `disk_status`、`disk_initialize`、`disk_read`、`disk_ioctl`。
- diskio 本身不操作 GPIO、不写 OV5640 寄存器、不执行重复 takeover。
- 外层 `Camera_SDStorage_CheckFatfsMount()` 统一拥有 camera pause、guard、DVP mask、SDIO takeover、`HAL_SD_Init`、mount 和 cleanup 生命周期。
- `disk_initialize` 只确认外层会话和 SD 卡 transfer 状态，不重复执行 `HAL_SD_Init`。
- `disk_read` 调用 `HAL_SD_ReadBlocks` polling，并在读前、读后等待 `HAL_SD_CARD_TRANSFER`；不使用 DMA 或 IRQ。
- `disk_ioctl` 最小支持 `CTRL_SYNC`、`GET_SECTOR_COUNT`、`GET_SECTOR_SIZE`、`GET_BLOCK_SIZE`。
- 只读配置下不实现 `disk_write`，也不调用 `HAL_SD_WriteBlocks`。

### 4. `ffconf.h` 最小只读配置

```text
FF_FS_READONLY  = 1
FF_USE_MKFS     = 0
FF_USE_LFN      = 0
FF_FS_REENTRANT = 0
FF_VOLUMES      = 1
FF_MIN_SS       = 512
FF_MAX_SS       = 512
FF_CODE_PAGE    = 437
FF_FS_EXFAT     = 0
```

CMake 仅加入 `ff.c` 和 FatFs include 路径；`ffsystem.c`、`ffunicode.c` 不参与当前构建。

### 5. `SD STATUS` 最小挂载检查

现有 `SD STATUS` 命令现在先调用 `Camera_SDStorage_CheckFatfsMount()`，再输出结果。没有增加新 CLI。内部顺序为：

1. 激活 snapshot guard，并通过现有 snapshot control 暂停 DCMI。
2. 保存 OV5640 `0x3018`，写入 `saved_3018 & 0x8F`。
3. 将 PC8/PC9/PC10/PC11/PC12/PD2 切换为 SDIO AF12。
4. 使能 SDIO clock，执行 `HAL_SD_Init` 和 card info 检查。
5. 激活只读 diskio 会话，执行 `f_mount(..., opt=1)`。
6. 记录 `last_mount=PASS/FAIL`。
7. 执行 `f_mount(NULL, "", 0)`，随后 deinit SD、关闭 clock、恢复 GPIO、恢复 `0x3018`、解除 guard。
8. 最终更新 `card_ready` 和 `last_error`。

挂载检查结束后已卸载并 deinit，所以 `sdio_ready=0`、`fatfs_ready=0` 是预期值；`card_ready=1` 表示最近一次完整 SD + FatFs 检查成功。
如果进入检查前 DCMI 正在连续向 LCD 输出，cleanup 会记录该状态并恢复默认 `480x320` 显示链路；PC dump 模式保持按请求启动下一帧采集。

最终简洁输出字段：

```text
SD STATUS:
  supported=
  card_ready=
  takeover_required=
  sdio_ready=
  fatfs_ready=
  last_mount=
  last_error=
  dvp_mask_solution=OV5640_3018_6_4
```

### 6. 错误清理与禁止写入

任一步失败均保留首个错误，并继续尽力执行 unmount、`HAL_SD_DeInit`、SDIO clock disable、GPIO restore、`0x3018` restore 和 guard release。完整 GPIO takeover 在验证前即标记为需要恢复，避免 AF12 部分切换失败时遗漏 PC10/PC12/PD2 清理。

本阶段不调用 `f_open`、`f_write`、`f_read` 或 `HAL_SD_WriteBlocks`，不创建文件，不启用 SDIO DMA/IRQ，不恢复任何 Stage 11C 诊断 CLI。

### 7. 判断标准与当前结果

- Debug 构建：PASS。
- `SD STATUS` 显示 `last_mount=PASS`：待硬件验证。
- `last_error=OK`：待硬件验证。
- 检查后 DUMP 正常：待硬件验证。
- basic/repeat 图像工具 PASS：待硬件验证。

Stage 13A-1 不执行硬件测试，也不提交 Git commit。

## Stage 13A-2 SD STATUS 自动挂载回归修复

### 1. 13A-1 板测结果

- `SD STATUS` 显示 `last_error=SDIO_HAL_INIT_FAILED`。
- `last_mount=NOT_RUN`，说明失败发生在 `HAL_SD_Init` 阶段，尚未执行 `f_mount`。
- 执行 `SD STATUS` 后，`DUMP` 和 binary 图像工具失败。

### 2. 判断

- 本次失败发生在 SD 初始化阶段，还未进入 FatFs 挂载流程。
- `SD STATUS` 自动触发硬件状态切换的设计不合理；板测已表明失败清理流程尚不能可靠恢复图像链路。
- `STATUS` 类命令必须保持只读，不应改变相机、GPIO、SDIO 或 FatFs 状态。

### 3. 修正

- `SD STATUS` 恢复为只读缓存状态显示，只调用 `Camera_SDStorage_GetStatus` 获取当前缓存值。
- `SD STATUS` 不再调用 `Camera_SDStorage_CheckFatfsMount`，因此不会触发停止 DCMI、DVP mask、SDIO takeover、`HAL_SD_Init` 或 `f_mount`。
- 保留已经接入并构建通过的 FatFs 源码、CMake 配置和内部 mount 函数，但该函数暂不从 CLI 启用；在清理与图像链路恢复经过板测验证前，不作为状态查询的一部分执行。
- 不新增 CLI，不恢复旧诊断 CLI，不写卡，不启用 SDIO DMA 或 IRQ。

### 4. 后续

- FATFS mount 测试应由最终功能流程触发，或在后续 SD snapshot 命令阶段统一处理。
- 在重新启用 mount 流程前，必须先完成失败路径清理与 DUMP/binary 图像链路恢复的板测验证。
- `STATUS` 类命令继续坚持只读原则。

## Stage 13B SD SNAPSHOT v1 固定文本文件写入测试

### 1. 本轮目的

- 新增最终功能命令 `SD SNAPSHOT`，由该命令执行第一次真实的 FatFs 文件写入。
- 本轮不保存图像，只创建或覆盖 FAT/FAT32 卡根目录中的 `SDTEST.TXT`。
- 文件内容是固定 ASCII 文本，用于先验证完整 SD 安全会话、FatFs 元数据更新和 polling 块写路径。

### 2. 为什么不使用 `SD STATUS`

- `STATUS` 类命令必须只读，不能因为查询状态而改变相机或存储硬件状态。
- `SD STATUS` 继续只调用 `Camera_SDStorage_GetStatus()` 读取缓存，不触发 DVP mask、SDIO takeover、`HAL_SD_Init`、`f_mount`、`f_open`、`f_write` 或 `HAL_SD_WriteBlocks`。
- 最近一次 `SD SNAPSHOT` 的 `last_snapshot`、`last_file`、`last_file_size`、`save_count` 和 `save_error` 仅作为缓存字段由 `SD STATUS` 显示。

### 3. FatFs 与 diskio 写配置

- `FF_FS_READONLY=0`，启用 FatFs 写 API。
- 保持 `FF_USE_MKFS=0`、`FF_USE_LFN=0`、`FF_FS_REENTRANT=0`、`FF_VOLUMES=1`、`FF_MIN_SS=512`、`FF_MAX_SS=512`、`FF_CODE_PAGE=437`、`FF_FS_EXFAT=0`。
- `FF_FS_NORTC=1`，文件时间戳使用 `ffconf.h` 的固定值，不依赖 RTC。
- `camera_fatfs_diskio.c` 实现标准 `disk_write`，映射到 `Camera_SDStorage_FatFsDiskWrite()`。
- 块写仅使用 polling `HAL_SD_WriteBlocks`，写前和写后都等待 `HAL_SD_CARD_TRANSFER`，超时或 HAL 失败会更新 `save_error` 和 `last_error`。
- `disk_write` 不切换 GPIO、不写 OV5640 寄存器、不执行 takeover，并受独立的 snapshot 写会话 guard 限制。

### 4. 内部流程

`Camera_SDStorage_SaveSnapshotText()` 按以下顺序执行：

1. 检查 software guard、SDIO GPIO、clock、FatFs session 和 DVP mask 均为空闲。
2. pause camera，激活 snapshot guard，并记录是否需要恢复连续 LCD capture。
3. 保存 OV5640 `0x3018`，写入 `saved_3018 & 0x8F` 完成 DVP mask。
4. 将 PC8/PC9/PC10/PC11/PC12/PD2 takeover 为 SDIO AF12。
5. 使能 SDIO clock，执行 polling 模式的 `HAL_SD_Init`。
6. 激活 FatFs disk session，执行 `f_mount(..., opt=1)`。
7. 激活仅限本次 snapshot 的 disk write guard。
8. 使用 `FA_CREATE_ALWAYS | FA_WRITE` 执行 `f_open("SDTEST.TXT")`。
9. 执行一次 `f_write`，检查 FatFs 返回值和完整写入字节数。
10. 进入统一 cleanup：`f_close`、关闭 write guard、`f_mount(NULL)`、`HAL_SD_DeInit`、关闭 SDIO clock、恢复 GPIO、恢复 `0x3018`、解除 snapshot guard，并按进入前状态恢复连续 LCD capture。

即使 prepare、DVP mask、takeover、SD init、mount、open、write 或 close 中任一步失败，也继续按已完成步骤尽力 cleanup。HAL init 已尝试但失败时也执行 `HAL_SD_DeInit`，避免残留 SDIO/HAL 状态影响后续 DUMP。

### 5. 固定文件内容与输出

`SDTEST.TXT` 使用 CRLF 行尾，内容为：

```text
ISP_OV5640 SD SNAPSHOT TEST
stage=13B
mode=text
frame=160x120
format=not_image_yet
result=PASS
```

成功时 `SD SNAPSHOT` 输出 `result=PASS`、`file=SDTEST.TXT`、实际字节数、`mount=PASS`、`write=PASS`、`cleanup=PASS`。失败时输出 `result=FAIL`、各阶段状态和具体 `error`；无论成功或失败都不会把该命令拆成临时诊断 CLI。

### 6. 判断标准

- `SD SNAPSHOT` 显示 `result=PASS`。
- `file=SDTEST.TXT`、`write=PASS`、`cleanup=PASS`。
- 执行后 `SD STATUS` 只读显示最近一次保存结果。
- 随后的 DUMP 正常。
- `tools/uart_image_request_basic.py` PASS。
- `tools/uart_image_request_repeat.py` PASS。
- 断电取卡后，电脑可看到并读取 `SDTEST.TXT` 的固定 ASCII 内容。

Codex 本轮只执行静态检查和 Debug 构建；以上 SD 卡写入、断电取卡、DUMP 和图像工具标准均待硬件验证。

### 7. 失败判断

- mount 失败时，优先确认 SD 卡已格式化为 FAT/FAT32；当前不支持 exFAT，也不调用 `f_mkfs`。
- write 失败时，检查 `disk_write`、polling `HAL_SD_WriteBlocks`、写前/写后 card-transfer 等待和超时结果。
- cleanup 失败时，根据 `error` 检查 `f_close`、unmount、HAL deinit、GPIO、OV5640 `0x3018` 和 snapshot guard 恢复步骤。
- cleanup 后 DUMP 或 binary 图像工具失败，说明相机恢复链路仍有问题，必须先修复恢复流程再继续图像保存。
- 本阶段不调用 `f_read`，不启用 SDIO DMA/IRQ，不触碰 PWDN/CAMOFF/CAMON，不写 OV5640 `0x3008` 或 `0x4202`，不修改图像协议或 Python 工具。

## Stage 13B-1 SD SNAPSHOT 初始化失败与恢复链路修复

### 1. 板测现象

- `SD SNAPSHOT` 返回 `result=FAIL`、`error=SDIO_HAL_INIT_FAILED`。
- `mount=NOT_RUN`、`write=NOT_RUN`，说明失败发生在 `HAL_SD_Init`，尚未进入 FatFs。
- 原输出显示 `cleanup=PASS`，但随后文本 `DUMP` 无响应，`uart_image_request_basic.py` 和 `uart_image_request_repeat.py` 均失败。
- `hook_fault`、UART DMA、stream overflow 和 IWDG 状态正常，因此原 `cleanup=PASS` 只是代码路径执行完成，不代表相机硬件链路真正恢复。

### 2. 判断

- 当前问题与 SD 卡是否为 FAT/FAT32 无关；mount、文件创建和写入都没有执行。
- Stage 13B 初版使用 `ClockDiv=118U` 和 SDIO GPIO `VERY_HIGH`，没有复用 Stage 11C-5T/5U 已经板测成功的 ATK1B 参数。
- 5T/5U 的有效路径是先完成 OV5640 `0x3018[6:4]` mask，再使用 SDIO `HIGH/PULLUP/AF12`、`ClockDiv=1U`、`SDIO_BUS_WIDE_1B` 执行 `HAL_SD_Init`、CardInfo 和 card TRANSFER 等待。
- 原 cleanup 只检查各清理函数的返回值，没有重新初始化并验证 DCMI，也没有提供独立的 restore 结果，因此不能作为 DUMP/图像恢复的充分证据。

### 3. SD 初始化修复

- `CAMERA_SD_INIT_CLOCK_DIV` 改为 `1U`。
- PC8/PC9/PC10/PC11/PC12/PD2 的 SDIO takeover 使用 `GPIO_MODE_AF_PP`、`GPIO_PULLUP`、`GPIO_SPEED_FREQ_HIGH`、`GPIO_AF12_SDIO`，与 ATK1B init 一致。
- 保持 `SDIO_BUS_WIDE_1B`，不调用 `HAL_SD_ConfigWideBusOperation`，不切换 4-bit。
- 保持 polling `HAL_SD_Init` 和 polling block I/O，不启用 SDIO DMA 或 SDIO IRQ。
- `HAL_SD_GetCardInfo` 成功后新增 `HAL_SD_CARD_TRANSFER` 等待；只有等待成功才设置 card info valid。
- DVP mask 仍只执行 `saved_3018 & 0x8F`，不写 `0x3008`、`0x4202`，不触碰 PWDN/CAMOFF/CAMON。

### 4. cleanup 与 restore 修复

统一 cleanup 按已进入的阶段尽力执行：

1. 文件已打开时执行 `f_close`。
2. 已尝试 mount 时执行 `f_mount(NULL, "", 0)`。
3. `HAL_SD_Init` 已尝试或 SDIO clock 已打开时执行 `HAL_SD_DeInit`。
4. 无条件关闭 SDIO clock，并核对 RCC 中的 SDIO enable bit 已清除。
5. 强制将 PC10/PC12/PD2 恢复为无上下拉输入，再把 PC8/PC9/PC11 恢复为 DCMI AF13。
6. 已保存 OV5640 `0x3018` 时写回原值并回读验证。
7. 调用 snapshot restore 解除 software guard。
8. 重新执行 `Camera_DCMI_Init()`；如果进入会话前是连续 LCD capture，则重新启动该链路。
9. 等待 100 ms，使 DCMI 和图像任务稳定。
10. 验证 guard 已解除、DVP mask 已清除、DCMI handle/DMA link 为 READY、PC8/PC9/PC11 为 AF13；发生过 takeover 时还验证 PC10/PC12/PD2 为安全输入。连续 capture 模式另外验证 DCMI CAPTURE 和 DMA stream 已重新使能。

`cleanup=PASS` 现在要求文件/FatFs 清理和全部硬件恢复检查均成功。`SD SNAPSHOT` 新增 `restore=PASS|FAIL`；即使原始错误是 `SDIO_HAL_INIT_FAILED`，恢复失败也会通过 `restore=FAIL` 和 `cleanup=FAIL` 单独暴露，不覆盖原始失败原因。

### 5. 命令边界

- `SD STATUS` 继续只调用 `Camera_SDStorage_GetStatus()` 显示缓存，不触发 DVP mask、takeover、`HAL_SD_Init`、`f_mount`、`f_open` 或 `f_write`。
- `SD SNAPSHOT` 继续作为唯一最终功能入口，不恢复 `SD ATK*`、`SD DVP*`、`SD READTEST`、`SD READINFO`、`SD LINESTATE`、`SD BUSWIDTH`、`SNAPSHOT PREPARE`、`SNAPSHOT RESTORE` 或 `IWDGTEST`。
- 本轮不修改 `camera_rtos.c/h`、Core、Python 工具或图像协议。

### 6. 判断标准

- 如果 SD 初始化仍失败，输出必须保持 `result=FAIL`、`error=SDIO_HAL_INIT_FAILED`，同时 `restore=PASS`、`cleanup=PASS`。
- 初始化失败或成功后，文本 DUMP 必须恢复正常。
- `tools/uart_image_request_basic.py` 必须 PASS。
- `tools/uart_image_request_repeat.py` 必须 PASS。
- 只有失败路径和成功路径的相机恢复均稳定后，才继续调试 FatFs mount/write。

Codex 本轮不执行硬件测试；DUMP、basic/repeat 和真实 SD 初始化结果均待开发板验证。

## Stage 13C SD SNAPSHOT v2 保存 RGB565 原始图像

### 1. Stage 13B 板测基线

Stage 13B 后续板测已经通过：`SD SNAPSHOT` 返回 `result=PASS`、`file=SDTEST.TXT`、`bytes=101`、`mount=PASS`、`write=PASS`、`cleanup=PASS`、`restore=PASS`。写卡后 DUMP、`uart_image_request_basic.py` 和 `uart_image_request_repeat.py` 也均 PASS，说明 ATK1B SDIO 初始化参数、FatFs 写入路径以及相机恢复链路可以作为 Stage 13C 的稳定基线。

### 2. v2 保存内容与固定规格

- `SD SNAPSHOT` 不再写固定测试文本，改为读取 `Camera_FrameBuffer_GetFrontFrame()` 返回的当前前台帧。
- 保存前校验前台帧指针非空，且规格严格为宽 160、高 120、RGB565 每像素 2 字节、总长度 38400 字节。
- 固定覆盖 SD 卡根目录中的 `IMAGE.RGB`，文件内容就是前台帧内存中的原始 RGB565 字节流。
- 不重新采集图像，不调用 UART dump，不执行灰度、二值化或其他图像处理，不转换 BMP，不增加 BMP 头，也不生成递增文件名。
- 本阶段不修改双缓冲模块；只调用既有前台帧读取接口。

### 3. 内部流程

1. 检查 SD snapshot 会话为空闲，并取得、校验当前前台帧。
2. pause camera 并激活 software guard；暂停后再次读取前台帧，确保最终写入指针在 SD 会话期间稳定。
3. 保存 OV5640 `0x3018` 并执行 `saved_3018 & 0x8F` DVP mask。
4. 按 Stage 13B 已验证的 ATK1B 参数执行 SDIO takeover、polling `HAL_SD_Init` 和 CardInfo/TRANSFER 等待。
5. 执行 `f_mount`，再以 `FA_CREATE_ALWAYS | FA_WRITE` 打开 `IMAGE.RGB`。
6. 使用一次 `f_write` 写入前台帧的 38400 字节，并同时校验 FatFs 返回值和实际写入字节数。
7. 无论成功或失败，均沿用统一的 `f_close`、unmount、HAL deinit、SDIO clock/GPIO、DVP `0x3018`、software guard、DCMI 和图像链路清理恢复流程。

### 4. CLI 与状态边界

- 不新增 CLI；HELP 仍只保留 `HELP`、`STATUS`、`PROC [BYPASS|GRAY|BINARY]`、`THR [0..255]`、`RESET`、`DUMP`、`SD STATUS`、`SD SNAPSHOT`。
- `SD STATUS` 保持纯只读缓存显示，不触发 DVP mask、SDIO takeover、`HAL_SD_Init`、`f_mount`、`f_open` 或 `f_write`。
- `SD SNAPSHOT` 成功时输出 `file=IMAGE.RGB`、`bytes=38400`、`format=RGB565`、`width=160`、`height=120`，并继续输出 mount、write、cleanup、restore 结果；失败时还输出具体 `error`。
- 不调用 `f_read` 或 `f_mkfs`；SD 底层仍只使用 polling block I/O，不启用 SDIO DMA 或 SDIO IRQ。

### 5. 判断标准

- `SD SNAPSHOT` 显示 `result=PASS`、`file=IMAGE.RGB`、`bytes=38400`、`format=RGB565`、`width=160`、`height=120`。
- `mount=PASS`、`write=PASS`、`cleanup=PASS`、`restore=PASS`。
- 断电取卡后，电脑可见 `IMAGE.RGB`，且文件大小严格为 38400 字节。
- 按 RGB565、160×120 解析后图像方向正确、颜色基本正确、内容与保存时前台帧一致。
- 保存后文本 DUMP、`uart_image_request_basic.py` 和 `uart_image_request_repeat.py` 均 PASS。

Codex 本轮只执行静态检查和 Debug 构建，不执行开发板、SD 卡、DUMP 或图像工具硬件测试。

## Stage 13C-1 IMAGE.RGB 全 0 修复

### 1. 板测现象与判断

- Stage 13C 初版 `SD SNAPSHOT` 返回 PASS，`IMAGE.RGB` 文件大小为 38400 字节，mount、write、cleanup、restore 以及保存后的 DUMP/basic/repeat 均正常。
- 断电取卡后发现 `IMAGE.RGB` 内容全为 `0x00`。这说明 FatFs、SDIO 写入和恢复链路已经成功，问题位于写卡前的图像数据源，而不能只用文件大小或 `bytes=38400` 判断保存成功。
- 初版在暂停相机后再次取得 front frame 并直接把该指针交给 `f_write`，没有验证源图像是否包含有效数据，也没有隔离 SD 会话期间的 buffer 生命周期。

### 2. 修复策略

- 与 DUMP 保持同源：继续使用已经由 `camera_pc_dump.c` 验证的 `Camera_FrameBuffer_GetFrontFrame()`，校验指针、160×120 尺寸和 38400 字节长度。
- 在 pause camera、DVP mask 和 SDIO takeover 之前，把当前 front frame 完整复制到文件作用域、4 字节对齐的 38400 字节 staging buffer；不在函数栈上分配大数组，也不使用 `malloc`。
- SD 会话中的 `f_write` 只读取 staging buffer，不再在暂停后重新获取 front frame，也不直接依赖可能变化的 frame-buffer 指针。
- 复制完成后逐字节计算 `source_nonzero` 和 `source_sum32`。由于 38400 个字节的最大和小于 `UINT32_MAX`，该统计不会发生 32 位溢出。
- 如果 front frame 无效则返回 `FRAME_BUFFER_INVALID`；如果 `source_nonzero==0` 或 `source_sum32==0`，返回 `FRAME_EMPTY`，保持 `mount=NOT_RUN`、`write=NOT_RUN`、`bytes=0`，且不进入相机暂停或 SD 写卡流程。
- `SD SNAPSHOT` 增加 `source=FRONT`、`source_bytes=38400`、`source_nonzero` 和 `source_sum32` 输出；`SD STATUS` 继续仅显示缓存状态，不触发任何硬件或 FatFs 流程。

### 3. 判断标准

- `SD SNAPSHOT` 返回 `result=PASS`、`file=IMAGE.RGB`、`bytes=38400`、`source=FRONT`、`source_bytes=38400`。
- `source_nonzero>0`、`source_sum32>0`，同时 `mount=PASS`、`write=PASS`、`cleanup=PASS`、`restore=PASS`。
- 断电取卡后 `IMAGE.RGB` 大小严格为 38400 字节，内容不是全 `0x00`。
- 按 160×120 RGB565 raw 转换后能够看到与保存时 front frame 对应的图像。
- 保存后文本 DUMP、`uart_image_request_basic.py` 和 `uart_image_request_repeat.py` 均 PASS。

Codex 本轮只执行静态检查与 Debug 构建；开发板、SD 卡内容、Python 转换以及 DUMP/basic/repeat 验证仍需板测完成。

## Stage 13C-3 抽取 DUMP 共用图像准备路径并供 SD SNAPSHOT 复用

### 1. Stage 13C-2 板测结果与审计结论

- 先执行 DUMP 再执行 `SD SNAPSHOT` 时，`source_nonzero=38400`、`source_sum32=5410716`，`IMAGE.RGB` 保存成功。
- 上电后不先执行 DUMP，直接执行 `SD SNAPSHOT` 时，front buffer 仍为空，`source_nonzero=0`。
- 审计确认文本 DUMP 和 binary image request 均由 `CameraServiceTask` 调用同一个内部请求入口；原入口在 `camera_rtos.c` 中依次执行 DCMI snapshot、等待完成、停止、front/back commit、当前 PROC 模式处理，最后才调用 `Camera_PC_Dump_SendFrame()` 发送 OV56RGB5。
- Stage 13C-2 的 SD SNAPSHOT 只读取已有 front buffer，没有执行上述图像准备步骤，因此依赖用户先运行 DUMP。

### 2. 公共图像准备接口

- 将原 DUMP 已验证的 capture、wait、stop、front/back commit 和 image process 逻辑抽取为 `Camera_RTOS_PrepareRgb565Frame(uint32_t timeout_ms)`。
- 公共函数保留原 DUMP 的 DCMI 启动错误、超时、commit 错误、图像处理错误和 BYPASS fallback 行为；成功返回后，现有 `Camera_FrameBuffer_GetFrontFrame()` 指向准备完成的 160×120 RGB565 数据。
- 公共函数不发送 UART、不操作 SD、不改变当前 PROC 模式，也不修改任务、优先级、栈或调度结构。
- `SD SNAPSHOT` 的 CLI 处理本来就在 `CameraServiceTask` 上下文中同步执行，因此直接调用公共 prepare，不新增任务、队列或跨任务 DCMI 访问。

### 3. DUMP 与 binary request 路径

- 原 DUMP 私有 capture/process/send 函数拆分为公共 prepare 和保留的内部 send wrapper。
- 文本 DUMP 与 binary image request 继续复用同一个请求入口：先调用 `Camera_RTOS_PrepareRgb565Frame()`，成功后再调用原 `Camera_PC_Dump_SendFrame()`。
- OV56RGB5 magic、version、pixel format、160×120 尺寸、38400 字节 payload、frame ID、CRC32、UART 分块发送和错误映射均保持不变。
- binary image request 的解析、seq、统计和响应协议保持不变。

### 4. SD SNAPSHOT 路径

1. 在 software guard、DVP mask、SDIO takeover 和 FatFs 会话之前调用 `Camera_RTOS_PrepareRgb565Frame()`。
2. 通过与 DUMP 相同的 `Camera_FrameBuffer_GetFrontFrame()` 获取准备后的 front buffer。
3. 校验指针、160×120 尺寸和 38400 字节长度，再复制到文件作用域、4 字节对齐的 static staging buffer。
4. 统计 staging buffer 的 `source_nonzero` 和 `source_sum32`。
5. 如果捕获或处理失败，返回 `FRAME_PREPARE_FAILED`；如果等待超时，返回 `FRAME_PREPARE_TIMEOUT`，均不进入 SD 写卡流程。
6. 如果准备成功但源仍为空，间隔 75 ms 重新执行同一公共 prepare，最多重试 3 次；仍为空则返回 `FRAME_EMPTY`，保持 mount/write 为 `NOT_RUN`。
7. 图像有效后才进入原有 DVP mask、ATK1B SDIO takeover、FatFs mount/write 和完整 cleanup/restore 流程。

`SD SNAPSHOT` 新增 `prepare=PASS|FAIL|NOT_RUN` 和 `prepare_retry=0..3` 输出。SD storage 不调用 DUMP 命令处理函数、`Camera_PC_Dump_SendFrame()` 或任何 UART 图像发送函数。

### 5. 判断标准

- 上电后不执行 DUMP，直接执行 `SD SNAPSHOT` 也应返回 `result=PASS`、`prepare=PASS`。
- `source_nonzero>0`、`source_sum32>0`、`bytes=38400`、mount/write/cleanup/restore 均 PASS。
- 断电取卡后 `IMAGE.RGB` 严格为 38400 字节且内容非全零，按 160×120 RGB565 raw 转换后图像可见。
- 保存后的文本 DUMP、binary basic/repeat 工具仍 PASS，OV56RGB5 和 binary image request 协议均无变化。

Codex 本轮只执行静态检查与 Debug 构建；上电直达 SD SNAPSHOT、SD 卡内容及 DUMP/basic/repeat 仍需开发板验证。

## Stage 13D SD SNAPSHOT v3 保存 BMP 图像

### 1. Stage 13C-3 基线与本轮目标

- Stage 13C-3 已实现 `SD SNAPSHOT` 自主调用 `Camera_RTOS_PrepareRgb565Frame()`，无需先执行 DUMP 即可准备有效图像并保存 `IMAGE.RGB`。
- 板测确认 `IMAGE.RGB` 不再全零，转换为 PNG 后图像正常；保存后的 DUMP 和 binary basic/repeat 工具均 PASS。
- Stage 13D 将固定输出升级为电脑可直接打开的 `IMAGE.BMP`，不再创建或覆盖 `IMAGE.RGB`。

### 2. BMP 文件格式

- 图像尺寸固定为 160×120，输出格式为未压缩 24-bit BMP，像素顺序为 BGR888。
- BITMAPFILEHEADER 为 14 字节，BITMAPINFOHEADER 为 40 字节，像素数据偏移为 54 字节。
- height 写为 `-120`，使用 top-down BMP，因此 staging buffer 的第 0 行直接写为 BMP 的第 0 行，不需要倒序遍历。
- 每行有效数据为 `160×3=480` 字节；480 已经是 4 字节对齐值，所以 row stride 也是 480 字节且没有额外 padding。
- 像素数据长度为 `480×120=57600` 字节，最终文件大小固定为 `54+57600=57654` 字节。
- BMP header 使用 54 字节 `uint8_t` 缓冲手工按 little-endian 填充，不直接写入可能含结构体 padding 的 C struct。

### 3. RGB565 转 BGR888

- 输入仍为 DUMP 同源的 160×120 RGB565 little-endian front frame，并在 SD 会话前复制到现有 38400 字节 static staging buffer。
- 每个像素按 `src[0] | (src[1] << 8)` 组成 RGB565，再分别提取 R5、G6、B5。
- 颜色扩展使用 `r8=(r5<<3)|(r5>>2)`、`g8=(g6<<2)|(g6>>4)`、`b8=(b5<<3)|(b5>>2)`，按 B、G、R 顺序写入 BMP 行缓冲。
- 保留现有 `source_nonzero`、`source_sum32`、prepare/retry 检查；源图像无效时不进入 SDIO/FatFs 会话。

### 4. RAM 与写入策略

- 保留现有文件作用域、4 字节对齐的 38400 字节 RGB565 staging buffer，保证 SD 会话期间图像源稳定。
- 新增文件作用域、4 字节对齐的 480 字节 BMP row buffer；另有 54 字节 BMP header buffer。
- 不分配 57600 字节 BMP 全图缓冲，不使用 `malloc`，也不在函数栈上放置 480、38400 或 57600 字节数组。
- 文件打开后先 `f_write` 54 字节 header，再逐行转换并分别 `f_write` 120 个 480 字节行；每次均核对 FatFs 返回值和实际写入长度。
- 只有累计写入严格等于 57654 字节时 write 才能 PASS；随后继续执行既有 close、unmount、deinit、DVP/GPIO/相机链路 cleanup/restore。

### 5. CLI 与状态边界

- HELP 命令列表不变，不新增 CLI；`SD SNAPSHOT` 输出更新为 `file=IMAGE.BMP`、`format=BMP24`、`bytes=57654`。
- `SD STATUS` 继续只读取缓存，不触发 prepare、DVP mask、SDIO takeover、HAL SD、FatFs 或块写入。
- 不调用 `f_read`、`f_mkfs`，不启用 SDIO DMA/IRQ，不修改 DUMP/OV56RGB5 或 binary image request 协议。

### 6. 判断标准

- `SD SNAPSHOT` 返回 `result=PASS`、`file=IMAGE.BMP`、`bytes=57654`、`format=BMP24`。
- prepare、mount、write、cleanup、restore 均 PASS，且 `source_nonzero>0`、`source_sum32>0`。
- 断电取卡后电脑可直接打开 `IMAGE.BMP`，图像尺寸为 160×120、方向正确、颜色基本正确。
- 保存后文本 DUMP 和 binary basic/repeat 工具仍 PASS。

Codex 本轮只执行静态检查与 Debug 构建；实际 SD 卡 BMP 打开、方向、颜色以及保存后 DUMP/basic/repeat 仍需开发板验证。

## Stage 13E SD SNAPSHOT 稳定性与耗时统计

### 1. Stage 13D 基线与本轮边界

- Stage 13D 已通过板测：`SD SNAPSHOT` 成功保存 160×120、BMP24、57654 字节的 `IMAGE.BMP`，电脑可直接打开。
- 最近一次状态显示 mount、write、cleanup、restore 均 PASS，保存后的 SD STATUS 缓存正确。
- Stage 13E 只增加最小耗时统计，不改变图像准备、BMP 转换、DVP mask、SDIO/FatFs、cleanup/restore 或固定文件覆盖行为，也不新增 CLI。

### 2. 耗时字段定义

- `total_ms`：从 `Camera_SDStorage_SaveSnapshotFrame()` 入口开始，到统一 cleanup 和相机链路 restore 完成。
- `prepare_ms`：调用公共 `Camera_RTOS_PrepareRgb565Frame()`、必要重试、front frame staging 复制及源数据统计所用时间。
- `write_ms`：从开始写 BMP 起，到 54 字节 header 和 120 个 480 字节 BGR888 行全部写入或发生写错误为止。
- `cleanup_ms`：从统一 cleanup 入口开始，包含必要的 file close、unmount、HAL SD deinit、SDIO clock/GPIO、DVP `0x3018` 和相机链路 restore。
- 全部字段使用 `HAL_GetTick()` 的无符号差值计算；失败时也输出已经执行阶段的耗时，未进入的阶段保持 0。

### 3. CLI 与缓存状态

- `SD SNAPSHOT` 在原输出末尾增加 `total_ms`、`prepare_ms`、`write_ms`、`cleanup_ms`，成功和失败路径均输出。
- `SD STATUS` 增加只读缓存字段 `last_total_ms` 和 `last_write_ms`，对应最近一次 snapshot 的 total/write 结果。
- 初始化时最近耗时字段为 0；每次 snapshot 完成或被 busy 条件拒绝时更新缓存。
- `SD STATUS` 仍只调用 `Camera_SDStorage_GetStatus()`，不触发 prepare、DVP mask、SDIO takeover、HAL SD、FatFs 或块写入。

### 4. 稳定性判断标准

- `SD SNAPSHOT` 持续返回 `result=PASS`，`IMAGE.BMP` 可正常打开且 `total_ms`、`prepare_ms`、`write_ms`、`cleanup_ms` 数值合理。
- 连续多次执行后，`save_count` 正确递增，`last_total_ms`、`last_write_ms` 更新为最近一次结果。
- 每次保存后文本 DUMP 正常，binary basic/repeat 工具 PASS。
- STATUS 中无新增 `hook_fault`、`uart_dma_error`、`stream_overflow` 或 IWDG `refresh_skip`。
- 本轮不调用 `f_read`、`f_mkfs`，不启用 SDIO DMA/IRQ，不修改 DUMP/OV56RGB5 或 binary image request 协议。

Codex 本轮只执行静态检查与 Debug 构建；连续 snapshot、实际耗时分布、保存后 DUMP/basic/repeat 和健康状态仍需开发板验证。

## Stage 13F SD SNAPSHOT BMP 文件递增命名

### 1. 基线与目标

- Stage 13D/13E 已验证固定覆盖 `IMAGE.BMP` 的 BMP24 保存、耗时统计以及保存后的相机链路恢复稳定。
- Stage 13F 只把输出文件名改为根目录短文件名 `IMG0001.BMP` 至 `IMG9999.BMP`；BMP24 内容、160×120 尺寸、57654 字节大小、公共 prepare 路径、DVP mask、SDIO/FatFs 会话和 cleanup/restore 流程均保持不变。
- 每次 snapshot 会话都从编号 0001 开始检查 SD 卡现有文件，不依赖 RAM 中保存的编号，因此重新上电后仍能继续选择第一个未占用名称。

### 2. 文件名选择流程

1. `f_mount` 成功后、`f_open` 之前，依次生成大写 8.3 文件名 `IMG0001.BMP` 至 `IMG9999.BMP`。
2. 对每个候选名称调用 `f_stat`：返回 `FR_OK` 表示已存在并继续检查；返回 `FR_NO_FILE` 表示选中该名称；其他返回值以 `FILE_SCAN_FAILED` 结束且不写卡。
3. 如果 0001 至 9999 全部存在，则返回 `FILE_INDEX_FULL`，不打开或覆盖任何文件。
4. 选中候选名称后使用 `FA_CREATE_NEW | FA_WRITE` 打开，进一步保证现有文件不会被覆盖。

### 3. 边界与状态

- 不启用 LFN，不创建目录，不使用 RTC 或时间戳；不调用 `f_read`、`f_mkfs`，不增加 SDIO DMA/IRQ，也不增加大图像缓冲区。
- 不新增 CLI；`SD STATUS` 继续只读显示缓存状态，`last_file` 和 `last_file_size` 保留最近一次成功保存的文件名与大小，失败检查不会抹掉该成功记录。
- DUMP/OV56RGB5、binary image request、公共 RGB565 prepare 路径以及 BMP header/逐行 RGB565 转 BGR888 写入逻辑均不改变。

### 4. 判断标准

- 空卡首次执行 `SD SNAPSHOT` 成功保存 `IMG0001.BMP`，再次执行成功保存 `IMG0002.BMP`，原文件保持不变。
- 卡中已有编号文件时选择第一个不存在的候选名称；扫描异常显示 `FILE_SCAN_FAILED`，编号耗尽显示 `FILE_INDEX_FULL`，两种失败均不写卡。
- 成功结果继续显示 `format=BMP24`、`bytes=57654`，prepare、mount、write、cleanup、restore 均为 PASS，耗时字段继续有效。
- 保存后文本 DUMP 正常，binary basic/repeat 工具 PASS；上述 SD 卡、图像和连续保存行为仍需开发板验证。
