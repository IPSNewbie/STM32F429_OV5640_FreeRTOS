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
