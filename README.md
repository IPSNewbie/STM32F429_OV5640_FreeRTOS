# STM32F429 + OV5640 FreeRTOS 嵌入式图像采集系统

基于 **STM32F429IGT6 + OV5640 + FreeRTOS** 实现的嵌入式图像采集与存储系统，覆盖摄像头驱动、DCMI/DMA 图像采集、图像处理、UART 通信、SDIO/FatFs 存储及系统可靠性设计。

当前稳定版本：**v1.0.0**

---

## 1. 项目架构

```text
                     PC
                      │
                UART RX DMA
                      │
                StreamBuffer
                      │
                      ▼
                  CommTask
                      │
                 CommandQueue
                      │
                      ▼
                ControlTask
               /      |       \
              /       |        \
             ▼        ▼         ▼
      CaptureTask  ProcessTask  StorageTask
        DCMI/DMA    图像处理     SDIO/FatFs
             \        |         /
              \       |        /
               └── Frame Buffer ┘

                      │
                      ▼
                 MonitorTask
             Heap / Stack / IWDG
```

### FreeRTOS 任务划分

* **CommTask**：UART DMA 接收、CLI 与协议解析
* **ControlTask**：命令调度与采集/处理/存储流程编排
* **CaptureTask**：DCMI + DMA 单帧采集
* **ProcessTask**：BYPASS / GRAY / BINARY 图像处理
* **StorageTask**：SDIO + FatFs BMP 图像保存
* **MonitorTask**：Heap、Stack、任务心跳及 IWDG 监控

任务间根据场景分别使用：

```text
StreamBuffer      UART 字节流
Queue             命令及任务请求/结果
Task Notification DCMI ISR → CaptureTask
EventGroup        系统状态与任务心跳
```

---

## 2. 核心功能

### OV5640 图像采集

* GPIO 软件模拟 SCCB 配置 OV5640
* DCMI + DMA 接收 RGB565 图像
* 双缓冲 Frame Buffer 管理
* 支持 160×120 图像采集与 PC 导出
* 支持 LCD 实时显示

### 图像处理

支持：

```text
BYPASS
GRAY
BINARY
```

二值化阈值可通过 CLI 在线修改。

### UART 通信

基于：

```text
UART DMA
    ↓
StreamBuffer
    ↓
CommTask
```

实现非阻塞串口接收。

支持文本 CLI 与二进制图像传输，PC 端可完成图像接收、CRC 校验及自动化稳定性测试。

### SD 卡拍照保存

支持：

```text
SD SNAPSHOT
```

自动完成：

```text
采集一帧
→ 图像处理
→ DVP释放
→ SDIO接管
→ FatFs写入BMP
→ 恢复摄像头
```

图片按照：

```text
IMG0001.BMP
IMG0002.BMP
...
```

顺序保存。

---

## 3. 关键工程问题

### DCMI 与 SDIO 引脚冲突

开发板中：

```text
PC8  : OV5640 D2 / SDIO D0
PC9  : OV5640 D3 / SDIO D1
PC11 : OV5640 D4 / SDIO D3
```

DCMI 与 SDIO 无法同时使用。

项目采用：

```text
停止采集
→ 屏蔽 OV5640 DVP 输出
→ GPIO 切换至 SDIO
→ 完成文件写入
→ 恢复 GPIO
→ 恢复 OV5640 DVP
```

实现摄像头与 SD 卡在共享引脚条件下可靠切换。

### FreeRTOS 下 SDIO Polling 随机失败

连续 SD 存储测试中曾出现：

```text
HAL_SD_ERROR_TX_UNDERRUN
HAL_SD_ERROR_RX_OVERRUN
```

定位发现 `HAL_SD_ReadBlocks()` / `HAL_SD_WriteBlocks()` 使用 polling 方式访问 SDIO FIFO，任务切换可能导致 CPU 无法及时服务 FIFO。

最终仅在底层 block 传输期间：

```c
vTaskSuspendAll();

/* HAL_SD_ReadBlocks / HAL_SD_WriteBlocks */

xTaskResumeAll();
```

暂停任务调度但保持中断开启，解决随机 FIFO Underrun / Overrun 问题。

---

## 4. 系统可靠性

MonitorTask 周期监控：

* FreeRTOS Heap
* 各任务 Stack High Water Mark
* UART DMA 错误
* StreamBuffer Overflow
* Assert / Hook Fault
* 多任务 Heartbeat

只有系统健康状态正常时才刷新 **IWDG**，任务异常时停止喂狗，由硬件看门狗完成系统复位。

---

## 5. 串口 CLI

当前仅保留必要命令：

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

避免保留大量仅用于开发阶段的诊断命令。

---

## 6. 硬件平台

| 模块   | 配置                      |
| ---- | ----------------------- |
| MCU  | STM32F429IGT6           |
| 开发板  | 正点原子阿波罗 V2              |
| 摄像头  | OV5640                  |
| 图像接口 | DCMI + DMA              |
| 图像格式 | RGB565                  |
| LCD  | 3.5 寸 MCU LCD / NT35310 |
| 存储   | Micro SD / SDIO / FatFs |
| 通信   | USART1 115200 8N1       |
| RTOS | FreeRTOS                |

OV5640 使用模块自带 **24 MHz** 晶振。

---

## 7. 主要代码

```text
BSPDrivers/
├── Inc/
└── Src/
    ├── camera_cli.c
    ├── camera_capture.c
    ├── camera_dcmi_dma.c
    ├── camera_frame_buffer.c
    ├── camera_image_process.c
    ├── camera_pc_dump.c
    ├── camera_process_task.c
    ├── camera_rtos.c
    └── camera_sd_storage.c

Middlewares/
└── Third_Party/
    ├── FreeRTOS/
    └── FatFs/

tools/
└── PC端图像与稳定性测试脚本
```

---

## 8. 项目特点

本项目重点不在单纯完成 OV5640 驱动，而在完整的嵌入式系统工程实践：

* **DCMI + DMA 图像采集**
* **FreeRTOS 多任务解耦**
* **StreamBuffer / Queue / Task Notification / EventGroup**
* **UART DMA 非阻塞通信**
* **双缓冲图像管理**
* **SDIO + FatFs BMP 存储**
* **DCMI / SDIO 共享引脚资源仲裁**
* **SDIO FIFO Underrun / Overrun 故障定位**
* **任务心跳 + MonitorTask + IWDG**
* **PC 自动化稳定性测试**

---

## 9. 项目状态

```text
OV5640 驱动             PASS
DCMI + DMA              PASS
LCD 显示                PASS
PC DUMP                  PASS
基础图像处理             PASS
UART DMA / CLI           PASS
FreeRTOS 多任务架构       PASS
SDIO / FatFs BMP 保存     PASS
EventGroup 状态管理       PASS
任务心跳 / IWDG           PASS
稳定性回归测试             PASS
```

当前正式版本：

```text
v1.0.0
```

项目已完成主要功能开发与 FreeRTOS 多任务架构重构，可用于嵌入式软件、STM32 外设驱动、RTOS 及嵌入式图像采集方向的学习与实践。
