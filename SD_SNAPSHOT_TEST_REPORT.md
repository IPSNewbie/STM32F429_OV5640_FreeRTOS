# SD Snapshot 测试报告

## 1. 测试目标

验证 STM32F429 + OV5640 项目在 DVP/SDIO 引脚复用条件下，能否稳定完成图像采集、BMP 转换、SD 卡保存、文件递增命名，并在 SD 会话结束后恢复摄像头链路。

## 2. 最终功能

1. 命令：`SD SNAPSHOT`。
2. 保存格式：BMP24。
3. 图像尺寸：160×120。
4. 文件大小：57654 bytes。
5. 文件命名：`IMG0001.BMP` 至 `IMG9999.BMP`。
6. 图像来源：与 DUMP 共用的 RGB565 图像准备路径。
7. `SD STATUS` 只读显示缓存状态。
8. SDIO 使用 1-bit polling 模式。
9. 不使用 SDIO DMA 或 SDIO IRQ。
10. 不调用 `f_read` 或 `f_mkfs`。

## 3. 最终命令列表

```text
HELP
STATUS
PROC [BYPASS|GRAY|BINARY]
THR [0..255]
RESET
DUMP
SD STATUS
SD SNAPSHOT
```

## 4. 关键问题定位

1. SD-only boot 下，ATK1B 1-bit polling 读取 block 0/2048 稳定 PASS，证明 SDIO 硬件、卡座、SD 卡和 HAL 初始化链路正常。
2. 正常摄像头环境中的 SDIO 失败不是 SDIO 硬件故障，而是 OV5640 DVP 数据线与 SDIO 数据线复用后仍被传感器持续驱动。
3. 关键共享线为 OV_D2/PC8/SDIO_D0、OV_D3/PC9/SDIO_D1、OV_D4/PC11/SDIO_D3，对应 OV5640 `0x3018[4]`、`[5]`、`[6]`。
4. SD 会话前保存 `0x3018` 原值，再写入 `saved_3018 & 0x8F`，屏蔽 DVP D2/D3/D4 输出并释放共享线。
5. SD 会话结束后恢复 `0x3018` 原值和 DCMI/GPIO 状态，保证图像链路恢复。
6. 主流程不使用 CAMOFF/PWDN、`0x3008` 或 `0x4202`。

## 5. 最终 SD SNAPSHOT 流程

1. 调用 `Camera_RTOS_PrepareRgb565Frame()` 主动准备一帧。
2. 获取 front RGB565 buffer。
3. 复制到 static staging buffer。
4. 检查 `source_nonzero` 和 `source_sum32`。
5. 保存并 mask OV5640 `0x3018[6:4]`。
6. 将共享 GPIO 切换到 SDIO AF12。
7. 执行 polling `HAL_SD_Init()`。
8. 执行 FatFs `f_mount()`。
9. 使用 `f_stat()` 查找第一个可用的 `IMGxxxx.BMP`。
10. 使用 `f_open()` 创建新文件，不覆盖已有文件。
11. 写入 54-byte BMP header。
12. 逐行执行 RGB565 到 BGR888 转换。
13. 使用 `f_write()` 写入 120 行像素数据。
14. 执行 `f_close()`。
15. 执行 `f_mount(NULL)` 卸载文件系统。
16. 执行 `HAL_SD_DeInit()` 并关闭 SDIO clock。
17. 恢复 GPIO/DCMI 配置。
18. 恢复 OV5640 `0x3018` 原值及相机链路。
19. 更新 `SD STATUS` 的只读缓存状态。

## 6. 测试结果

1. IMAGE.RGB 初版文件大小为 38400 bytes，但内容全为 `0x00`；根因是 SD SNAPSHOT 未主动准备图像。改为复用 DUMP 的 `Camera_RTOS_PrepareRgb565Frame()` 路径后问题闭环。
2. IMAGE.BMP 测试结果为 `result=PASS`、`bytes=57654`、`source_nonzero=38400`，prepare、mount、write、cleanup、restore 均为 PASS，电脑可直接打开图片。
3. 文件递增测试已正常生成 `IMG0001.BMP`、`IMG0002.BMP`、`IMG0003.BMP`、`IMG0004.BMP`；取卡检查文件顺序和图片内容正常。
4. `tools/uart_sd_snapshot_stability.py` 自动稳定性测试结果正常。具体次数和逐轮数据保存在 `captures/` 下的 CSV/log 中，不纳入仓库。
5. 保存后的 DUMP、binary basic/repeat、SD STATUS 和相机链路恢复结果正常。

## 7. 耗时统计

1. `total_ms` 典型约 829～1023 ms。
2. 首次保存可能约 1730 ms，主要开销来自首次 mount 和文件系统访问。
3. `prepare_ms` 约 117～166 ms。
4. `write_ms` 约 555～706 ms，首次写入可能更高。
5. `cleanup_ms` 约 131～137 ms。

## 8. 最终限制

1. 当前只保存 160×120 BMP24。
2. 文件编号上限为 `IMG9999.BMP`。
3. 当前使用 SDIO 1-bit polling，不使用 DMA/IRQ。
4. 每次 `SD SNAPSHOT` 都会短暂停止图像链路并执行 SDIO takeover。
5. `SD STATUS` 只显示缓存状态，不主动检测 SD 卡。
6. 当前不支持写入后的文件读回校验。
7. 当前不支持创建目录。
8. 当前不支持 RTC 时间戳命名。

## 9. 后续可优化项

1. 保存 320×240 或更高分辨率图像。
2. 评估使用 SDIO DMA 优化写入速度。
3. 增加文件夹分类。
4. 增加 RTC 时间戳命名。
5. 增加 BMP/JPEG/RAW 多格式选择。
6. 增加 SD 卡剩余空间检测。
7. 增加断电保护和写入失败恢复策略。

## 10. 项目简历描述草稿

基于 STM32F429 + OV5640 + FreeRTOS 实现图像采集与 SD 卡本地存储功能。针对 OV5640 DVP 数据线与 SDIO 数据线复用导致的读写失败问题，通过 SD-only boot 对照实验、DVP 输出寄存器定位和 `0x3018[6:4]` 软件隔离方案完成根因闭环；在不启用 SDIO DMA/IRQ 的条件下，基于 FatFs 实现 160×120 RGB565 图像采集、BMP24 转换、`IMGxxxx.BMP` 递增保存、状态统计和自动化稳定性测试，支持写入后摄像头链路恢复和 UART DUMP 回归验证。
