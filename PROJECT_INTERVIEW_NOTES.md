# 项目面试准备：STM32F429 + OV5640 图像采集与 SD 卡存储系统

## 1. 项目一句话

这是一个基于 STM32F429 + OV5640 + FreeRTOS 的嵌入式图像系统，完成了 DCMI/DMA 采集、双缓冲处理、UART 图像传输和 FatFs BMP 保存，并闭环解决了 DVP 与 SDIO 共享引脚冲突。

## 2. 项目整体架构怎么讲

1. **硬件层**：STM32F429、OV5640、NT35310 LCD、SD 卡和 UART，其中 DVP D2/D3/D4 与 SDIO D0/D1/D3 共享 PC8/PC9/PC11。
2. **驱动层**：HAL GPIO/UART/DCMI/DMA/SDIO，软件 SCCB 配置 OV5640，LCD 驱动负责本地显示，FatFs 提供文件系统能力。
3. **RTOS 任务层**：Camera task 处理图像请求和采集准备，monitor 维护运行状态与 IWDG；不在多个任务中并发操作 DCMI/frame buffer。
4. **图像处理层**：front/back 双缓冲管理 RGB565 帧，支持 BYPASS、GRAYSCALE、BINARY 和阈值调整。
5. **通信与存储层**：UART DUMP 使用 OV56RGB5 + CRC32；SD SNAPSHOT 使用 DVP mask、SDIO takeover 和 FatFs 保存 BMP24。
6. **PC 工具层**：Python 工具完成串口请求、CRC 校验、图像恢复、质量分析、连续请求和 SD Snapshot 稳定性测试。

## 3. 面试官可能问的问题

### 1. 为什么选择 STM32F429？

STM32F429 自带 DCMI、DMA、SDIO、FMC 和较丰富的 SRAM/外设资源，能同时覆盖摄像头采集、LCD 显示、UART 和 SD 卡。它的性能足以支撑本项目 160×120 RGB565 的采集、处理和存储，也适合展示多外设协同与 RTOS 管理能力。

### 2. OV5640 如何初始化？

先完成 PWDN/RESET 时序，再通过软件 SCCB 读取芯片 ID，确认返回 `0x5640`。之后写入工作模式、输出尺寸和 RGB565 相关寄存器表；模块自带 24 MHz 晶振，因此本项目不依赖 MCU XCLK。

### 3. DCMI/DVP 图像如何采集？

OV5640 通过 8-bit DVP 输出像素数据和 PCLK/VSYNC/HREF，STM32 DCMI 根据同步信号采样，再由 DMA 把数据搬到 frame buffer。上层等待帧完成后停止 snapshot，提交 front/back buffer，再做处理或发送。

### 4. 为什么要做双缓冲？

如果采集和发送/处理共用同一块内存，DMA 可能在 CPU 消费数据时覆盖画面，出现撕裂或竞态。front/back 双缓冲让 back 用于采集，完成后原子 commit 为 front，消费者始终读取稳定帧。

### 5. FreeRTOS 在项目中做了什么？

FreeRTOS 主要用于把摄像头服务与状态监控分离。Camera task 串行执行图像请求和 DCMI/frame buffer 操作，monitor 统计 heap、stack、fault 和 IWDG 状态，既保留正确执行上下文，也避免 CLI 任务并发碰硬件。

### 6. UART DUMP 协议如何设计？

文本 `DUMP` 只负责触发，图像使用二进制 OV56RGB5 帧输出。帧中包含 magic、版本、像素格式、宽高、payload 长度、frame ID、38400-byte RGB565 payload 和 CRC32，PC 端先校验 header/长度，再校验 payload CRC。

### 7. 图像为什么使用 RGB565？

RGB565 每像素 2 bytes，比 RGB888 节省三分之一内存和带宽，同时能直接匹配常见 MCU LCD。160×120 单帧只需 38400 bytes，适合 STM32F429 的 SRAM 和 UART/SD staging 需求。

### 8. 灰度和二值化怎么做？

先从 RGB565 提取 R/G/B 分量并计算亮度，再把亮度重新映射为灰色 RGB565。二值模式把亮度与 CLI 配置的 0～255 阈值比较，输出黑或白；BYPASS 则保留原图。

### 9. SD 卡为什么一开始失败？

正常摄像头环境下，OV5640 的 DVP D2/D3/D4 持续驱动 PC8/PC9/PC11，而这些引脚又是 SDIO D0/D1/D3。即使 MCU 把 GPIO 切到 SDIO，外部传感器仍在驱动线路，因此 `HAL_SD_Init()` 或数据传输会失败。

### 10. 怎么证明不是 SD 卡硬件问题？

我建立了不启动摄像头/DVP 的 SD-only boot 对照环境，保持同一块板、同一卡座和同一张卡。该环境下 `HAL_SD_Init()`、CardInfo 和 1-bit polling block read 稳定通过，因此硬件链路本身正常。

### 11. SD-only boot 实验说明什么？

它通过控制变量把摄像头从系统中移除。如果 SD-only PASS、正常摄像头环境 FAIL，差异变量就集中在摄像头初始化、DVP 输出和共享 GPIO，而不是 SD 卡或 HAL API。

### 12. DVP/SDIO 共享线具体冲突在哪里？

OV_D2 使用 PC8，同时是 SDIO_D0，对应 OV5640 `0x3018[4]`；OV_D3 使用 PC9，同时是 SDIO_D1，对应 `[5]`；OV_D4 使用 PC11，同时是 SDIO_D3，对应 `[6]`。这三条线必须在 SD 会话期间由传感器释放。

### 13. 为什么降低 SDIO 速度不能根治？

降速只能改善建立保持时间和信号裕量，不能解决两个器件同时主动驱动同一根线的问题。只要 OV5640 没释放输出，总线所有权冲突就存在，所以根治方案必须控制 DVP pad 输出。

### 14. 为什么不用 CAMOFF/PWDN？

CAMOFF/PWDN 属于更粗粒度的整机控制，恢复后可能需要重新走传感器初始化和图像稳定过程，也会扩大对已验证摄像头链路的影响。本项目选择直接控制对应 DVP pad，作用范围更小，并能保存和恢复原寄存器值。

### 15. 为什么最终使用 `0x3018[6:4]`？

引脚映射表明 `[4]`、`[5]`、`[6]` 正好控制与 SDIO 冲突的 D2/D3/D4 输出。写入 `saved_3018 & 0x8F` 只清除这三位，保留其他 pad 设置；会话结束再恢复 saved value，改动最小且可逆。

### 16. SD SNAPSHOT 的完整流程是什么？

先公共 prepare 一帧，复制 front buffer 并检查非零统计；再暂停相机、mask DVP、切换 SDIO GPIO、初始化 SD 和 mount FatFs。随后扫描递增文件名、创建 BMP、逐行转换写入，最后 close/unmount/deinit，恢复 GPIO、`0x3018` 和相机链路，并更新状态缓存。

### 17. 为什么要复用 DUMP 图像准备路径？

DUMP 的 capture/wait/commit/process 已经过板测验证。如果 SD SNAPSHOT 再复制一套流程，会产生不同状态机和维护成本；抽成公共 prepare 后，两种输出拿到的是同源有效 front frame，也避免并发访问 DCMI。

### 18. 为什么 `IMAGE.RGB` 一开始全 0？

文件大小和 FatFs 返回值都正确，说明写入链路成功，但当时 SD SNAPSHOT 直接读取尚未由 DUMP 准备过的 front buffer。加入公共 prepare、staging copy 和 `source_nonzero/source_sum32` 检查后，空帧会在进入 SD 会话前被拦截。

### 19. 为什么使用 BMP24？

BMP24 格式简单、无需压缩算法，PC 可以直接打开，也便于验证方向和颜色。项目将 RGB565 逐行转成 BGR888，只需要 54-byte header 和 480-byte 行缓冲，不必再申请 57600-byte 全图 buffer。

### 20. 为什么当前不用 SDIO DMA？

当前阶段优先完成共享线问题、FatFs 写入和恢复链路的正确性，1-bit polling 已能稳定保存。DMA 会引入 IRQ、buffer 生命周期和并发清理复杂度，所以把它保留为后续独立性能优化，而不是在根因未闭环时叠加变量。

### 21. FatFs 在项目中怎么用？

项目接入本地 FatFs，并实现最小 diskio 适配，底层调用 polling block read/write。snapshot 中使用 `f_mount`、`f_stat`、`f_open`、`f_write`、`f_close` 和 unmount，不使用 `f_read`、`f_mkfs`、LFN 或目录扫描。

### 22. 如何保证写卡后摄像头恢复？

所有步骤共用统一 cleanup 标签，按已执行状态尽力 close、unmount、HAL deinit、关时钟、恢复 GPIO、恢复 `0x3018` 和 DCMI。错误清理本身也记录首个错误，最终通过写卡后的 DUMP 和 binary request 验证恢复是否完整。

### 23. 如何做稳定性测试？

PC 脚本循环发送 `SD SNAPSHOT`，解析每轮 key/value 响应，检查 PASS、文件编号连续、57654-byte 大小、源数据非零和 mount/write/cleanup/restore。结束后再检查 STATUS 的 fault/UART/IWDG 和 SD STATUS 的错误缓存，并把逐轮数据写入 CSV/log。

### 24. 项目还有哪些不足？

目前分辨率只有 160×120，保存格式固定 BMP24，SDIO 使用 1-bit polling，且没有读回校验、目录管理、RTC 命名和 GUI。它更侧重完整链路和工程定位能力，还不是高吞吐量产品方案。

### 25. 如果继续优化，你会怎么做？

我会先用测量数据确定瓶颈，再分阶段尝试 320×240、SDIO DMA 和更大的连续写块；同时增加卡空间检测、读回校验、断电保护与 RTC/目录管理。协议侧可以补充结构化控制和 GUI，但每项都应单独回归 DUMP、SD 保存和恢复链路。

## 4. 最重要的技术难点讲法

### 4.1 DVP/SDIO 共享线冲突定位

- **现象**：正常摄像头工程中 SDIO 初始化/传输失败，修改 ClockDiv 和总线宽度不能稳定解决。
- **排查**：建立 SD-only boot，对同一硬件执行 ATK1B 1-bit polling block 0/2048 读取；再逐条核对原理图和 OV5640 pad 控制寄存器。
- **根因**：OV5640 DVP D2/D3/D4 持续驱动 PC8/PC9/PC11，与 SDIO D0/D1/D3 争用线路。
- **解决**：保存 `0x3018`，SD 会话前写入 `saved_3018 & 0x8F`，再切换 GPIO 到 AF12；会话后恢复原值。
- **验证**：SD SNAPSHOT 连续 PASS，文件可打开；写卡后的 DUMP、binary request 和图像链路均正常。

### 4.2 SD SNAPSHOT 图像为空问题

- **现象**：`IMAGE.RGB` 文件为 38400 bytes，写入结果 PASS，但内容全部为 `0x00`。
- **排查**：先利用文件大小和 FatFs 返回值排除写卡失败，再统计写入前 buffer 的 `source_nonzero/source_sum32`，并比较“先 DUMP”和“直接 snapshot”的差异。
- **根因**：snapshot 没有主动执行 capture/wait/commit/process，直接读取了尚未准备好的 front buffer。
- **解决**：抽取 `Camera_RTOS_PrepareRgb565Frame()`，让 DUMP 和 SD SNAPSHOT 共用；准备后复制到 static staging buffer，空帧不写卡。
- **验证**：上电后无需先 DUMP 即可保存非零图像，BMP 可打开，源统计和各阶段状态均 PASS。

### 4.3 写卡后恢复图像链路

- **现象**：早期 SD 检查失败后，后续 DUMP 和图像工具也失败，说明 STATUS/SD 流程残留了硬件状态。
- **排查**：逐项审计 DCMI pause、software guard、GPIO AF、SDIO clock/HAL、DVP mask 的成功和失败出口。
- **根因**：部分失败路径未完整恢复 GPIO、`0x3018` 或 DCMI/DMA 状态，STATUS 类命令还错误触发了硬件切换。
- **解决**：`SD STATUS` 改为纯缓存只读；真正 snapshot 统一使用带状态标志的 cleanup/restore，所有出口尽力逆序释放资源。
- **验证**：snapshot 成功和失败路径均输出 cleanup/restore，保存后文本 DUMP、binary basic/repeat 及健康状态正常。

## 5. 项目中能体现的能力

1. STM32 HAL 多外设开发：GPIO、UART、DCMI、DMA、SDIO、IWDG。
2. FreeRTOS 基础任务划分、执行上下文约束和运行健康监控。
3. UART 文本 CLI、二进制帧协议、CRC32 和 PC 工具协同开发。
4. 图像格式、front/back buffer 和 staging buffer 生命周期管理。
5. SDIO block I/O、FatFs diskio 适配和 BMP 文件组织。
6. 使用对照实验、原理图映射、寄存器验证和失败路径审计定位嵌入式问题。
7. Python 自动化回归、CSV/log 归档和量化耗时统计。
8. Git 分支、Stage 拆分、小步改动和阶段文档化开发。

## 6. 项目不足与改进

1. 当前分辨率较低，只验证到 160×120；后续需要评估 SRAM、采集时间和写入带宽后再提高分辨率。
2. SDIO 未使用 DMA，CPU 占用和写入时间仍有优化空间，但引入 DMA 前需要先设计 IRQ 和 buffer 生命周期。
3. BMP24 无压缩、文件相对较大；可评估 JPEG，但会增加编码性能和内存压力。
4. 当前没有文件系统读回校验，只验证 FatFs 写入结果和取卡检查；可增加可选 CRC/读回流程。
5. 当前没有 RTC 时间戳和目录分类，文件名最多到 `IMG9999.BMP`。
6. 当前没有完整图形化上位机，PC 端以命令行脚本、图像文件和 CSV/log 为主。
