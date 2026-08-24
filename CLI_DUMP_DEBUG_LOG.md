# CLI 与 DUMP 命令异常排查记录

## 1. 问题背景

本阶段在 STM32F429 + OV5640 项目中新增串口 CLI 在线调参功能，目标是在不重新烧录固件的情况下，通过串口命令切换图像处理模式，例如：

```text
HELP
STATUS
PROC BYPASS
PROC GRAY
PROC BINARY
THR 128
RESET
DUMP
```

其中 `DUMP` 命令用于 PC 端抓取当前图像帧，协议保持原有二进制格式：

```text
OV56RGB5 + header + RGB565 payload + CRC
```

测试过程中发现：

```text
开发板上电后，直接运行 Python DUMP 工具，可以正常收到图像。

但是使用旧串口助手打开串口、关闭串口后，再运行 Python DUMP 工具，会出现 DUMP 超时，PC 端收不到 OV56RGB5 图像包。
```

Python 工具报错现象类似：

```text
OV56RGB5 not received after 3 DUMP attempts
```

该问题最初容易误判为：

```text
HAL_UART_Receive 接收异常
串口 line buffer 没清空
DUMP 命令解析失败
图像处理流程卡住
PC Dump 协议错误
```

后续通过逻辑分析仪、CLion + ST-Link 调试、原理图分析和串口工具对比，最终确认问题根因不是固件的 DUMP 协议，而是旧串口助手打开/关闭串口时影响 CH340 的 DTR/RTS 控制线，触发开发板一键下载电路，使 STM32 进入 System Memory Bootloader。

---

## 2. 排查目标

本次排查主要确认以下几个问题：

```text
1. PC 是否真的发送了 DUMP 命令。
2. STM32 是否真的收到了 DUMP 命令。
3. STM32 是否运行在用户程序中。
4. DUMP 无响应时，问题发生在 PC 端、串口链路、UART 接收层，还是 MCU 启动模式。
5. 串口助手打开/关闭串口是否会改变开发板启动状态。
```

---

## 3. 排查手段一：基准 DUMP 测试

首先不打开旧串口助手，开发板正常上电后，直接运行 Python 抓图工具：

```bash
python tools/pc_dump_rgb565.py --port COM6 --baud 115200 --tag direct_dump
```

测试结果：

```text
Python 能够收到 OV56RGB5 图像包。
图像可以正常保存为 PNG。
summary.csv 中可以生成亮度、RGB 均值、Laplacian 等统计结果。
```

该测试说明：

```text
1. Python 抓图工具正常。
2. COM 口基础通信正常。
3. STM32 的 DUMP 主流程正常。
4. OV56RGB5 协议正常。
5. 图像采集、帧缓存、图像处理、图像发送链路没有整体性故障。
```

---

## 4. 排查手段二：逻辑分析仪抓取 UART 波形

为了区分是 PC 没有发送命令，还是 STM32 没有响应，使用逻辑分析仪抓取 USART1 的 TX/RX 信号。

接线方式：

```text
CH0 -> STM32 TX / USB-UART RXD
CH2 -> STM32 RX / USB-UART TXD
GND -> GND
```

串口解码参数：

```text
Baudrate: 115200
Data bits: 8
Stop bits: 1
Parity: None
Bit order: LSB first
Idle level: High
Invert: No
```

### 4.1 正常场景波形

操作：

```text
1. 开发板上电。
2. 不打开旧串口助手。
3. 直接运行 Python DUMP 工具。
4. 使用逻辑分析仪观察 CH2 和 CH0。
```

观察结果：

```text
CH2 能看到 PC 发出的 DUMP\n。
CH0 能看到 STM32 返回 OV56RGB5 图像包。
Python 能成功保存图像。
```

波形截图：

```markdown
![正常 DUMP：PC 发送 DUMP，STM32 返回 OV56RGB5](docs/images/cli_dump_debug/normal_dump_uart.png)
```

结论：

```text
正常场景下，PC 到 STM32 的 RX 链路正常，STM32 到 PC 的 TX 链路正常，DUMP 协议正常。
```

### 4.2 异常场景波形

操作：

```text
1. 开发板重新上电。
2. 打开旧串口助手。
3. 关闭旧串口助手。
4. 运行 Python DUMP 工具。
5. 使用逻辑分析仪观察 CH2 和 CH0。
```

观察结果：

```text
CH2 能看到 PC 发出的 DUMP\n。
CH0 没有看到 STM32 返回 OV56RGB5。
Python DUMP 超时。
```

波形截图：

```markdown
![异常 DUMP：PC 发送 DUMP，但 STM32 没有返回数据](docs/images/cli_dump_debug/failed_dump_uart.png)
```

该结果说明：

```text
PC 并不是没有发送 DUMP。
USB 转串口到 STM32 RX 的物理链路并不是完全断开。
问题在于：STM32 当时没有按照用户程序的 DUMP 协议进行响应。
```

此时问题范围从“PC 工具问题”缩小到“STM32 当前运行状态或 UART 外设状态问题”。

---

## 5. 排查手段三：CLion + ST-Link 暂停查看程序位置

为了确认 MCU 当时到底运行在哪里，使用 CLion + ST-Link 进行在线调试。

操作方法：

```text
1. 使用 ST-Link 启动 Debug。
2. 点击 Resume / F9，让程序全速运行。
3. 复现 DUMP 异常。
4. 在 Python DUMP 超时等待期间，点击 Pause。
5. 查看当前 PC 地址和 Call Stack。
```

### 5.1 正常场景暂停结果

正常运行时，暂停后可以看到程序停在用户固件相关函数中，例如：

```text
Camera_PC_Dump_WaitForCommand()
HAL_UART_Receive()
UART_WaitOnFlagUntilTimeout()
main()
```

这种情况说明：

```text
MCU 正在运行用户程序。
程序正在等待串口接收新的命令字节。
```

此时程序位置属于正常用户固件流程。

CLion 正常场景截图：

```markdown
![正常场景暂停：程序停在 HAL_UART_Receive 等待串口字节](docs/images/cli_dump_debug/clion_wait_uart_normal.png)
```

### 5.2 异常场景暂停结果

旧串口助手打开/关闭后，再运行 Python DUMP 失败，此时点击 CLion Pause，发现 PC 地址位于：

```text
0x1FFF1074
```

而不是用户 Flash 区域。

STM32F429 用户程序正常运行地址通常在：

```text
0x08000000 附近
```

而：

```text
0x1FFFxxxx
```

属于 STM32 内部 System Memory 区域，也就是芯片内部 ROM Bootloader / ISP 启动程序所在区域。

CLion 异常场景截图：

```markdown
![异常场景暂停：PC 位于 0x1FFFxxxx System Memory](docs/images/cli_dump_debug/clion_pc_1fff_bootloader.png)
```

结论：

```text
旧串口助手打开/关闭后，STM32 没有继续运行用户程序，而是进入了 System Memory Bootloader。
```

这可以解释前面所有异常现象：

```text
1. PC 发出了 DUMP，但 STM32 没有返回 OV56RGB5。
2. 因为此时运行的不是用户固件，而是芯片内部 Bootloader。
3. Bootloader 不认识本项目定义的 DUMP 命令。
4. 因此 Python 等不到 OV56RGB5 图像包。
```

---

## 6. 排查手段四：查看 USB 转串口与一键下载电路原理图

根据开发板 USB USART & USB POWER 原理图，板载 USB 转串口芯片为 CH340C。

CH340C 除了 TXD/RXD 外，还有：

```text
RTS#
DTR#
```

这两个握手控制信号通过三极管电路连接到：

```text
RESET
BOOT0
```

相关电路作用是实现“一键下载”。

一键下载电路的基本目的：

```text
通过 PC 下载工具自动控制 DTR/RTS，使开发板自动完成：
1. BOOT0 拉高；
2. MCU 复位；
3. 复位释放后从 System Memory 启动；
4. 进入 STM32 内部 Bootloader；
5. 通过串口下载程序。
```

也就是说，用户不需要手动按 BOOT0 或 RESET，下载工具就能自动让 MCU 进入 ISP 下载模式。

这对下载程序很方便，但对本项目的串口 CLI 调试会造成影响：

```text
如果普通串口助手在打开/关闭串口时改变 DTR/RTS 电平，就可能误触发一键下载电路。
```

---

## 7. DTR/RTS 是什么

DTR 和 RTS 是串口通信中常见的控制信号。

```text
DTR = Data Terminal Ready
RTS = Request To Send
```

它们原本用于串口设备之间的状态通知和硬件流控。

在很多 USB 转串口芯片中，例如 CH340、CP2102、FT232，除了 TXD/RXD 数据线外，也会提供 DTR/RTS 这类控制线。

在普通串口通信中，只需要：

```text
TXD
RXD
GND
```

但是在单片机开发板中，DTR/RTS 经常被额外用于自动复位和自动进入下载模式。

本开发板中，DTR/RTS 不是单纯的串口流控信号，而是参与了一键下载电路。

---

## 8. 为什么串口助手打开/关闭时会改变 DTR/RTS

很多 PC 串口软件在打开 COM 口时，会自动初始化串口状态。

这个初始化过程可能包括：

```text
1. 设置波特率；
2. 设置数据位、停止位、校验位；
3. 设置流控模式；
4. 设置 DTR 状态；
5. 设置 RTS 状态。
```

即使用户没有主动点击 DTR 或 RTS，串口软件底层调用 Windows 串口 API 打开端口时，也可能默认改变 DTR/RTS。

不同串口软件行为不完全一样：

```text
有的软件打开串口时会自动拉高 DTR/RTS。
有的软件关闭串口时会释放 DTR/RTS。
有的软件会短暂翻转 DTR/RTS。
有的软件允许手动关闭 DTR/RTS。
有的软件界面上不显示 DTR/RTS，但内部仍可能改变它们。
```

对于普通串口设备，这通常没有明显影响。

但对于带一键下载电路的 STM32 开发板，DTR/RTS 的变化可能通过三极管网络影响 BOOT0 和 RESET。

如果在某个时刻形成如下组合：

```text
BOOT0 被拉高
RESET 被触发
```

那么 STM32 复位后不会从用户 Flash 启动，而会从 System Memory 启动，进入内部 Bootloader。

这就是旧串口助手打开/关闭后 DUMP 失效的原因。

---

## 9. 为什么 MobaXterm 可以正常使用

更换为 MobaXterm Serial 后，设置为：

```text
Serial port: COM6
Speed: 115200
Data bits: 8
Stop bits: 1
Parity: None
Flow control: None
```

测试流程：

```text
1. 开发板上电。
2. 在 MobaXterm 中发送 STATUS。
3. CLI 能正常返回状态信息。
4. 关闭 MobaXterm。
5. 运行 Python DUMP 工具。
6. Python 能成功收到图像。
```

测试命令：

```bash
python tools/pc_dump_rgb565.py --port COM6 --baud 115200 --tag moba_after_close_dump
```

测试结果：

```text
MobaXterm 打开/关闭串口后，STM32 没有进入 0x1FFFxxxx Bootloader。
Python DUMP 可以正常收到 OV56RGB5 图像包。
```

因此，后续 CLI 文本命令测试推荐使用：

```text
MobaXterm Serial
COM6
115200
Flow control = None
```

不再使用会触发一键下载电路的旧串口助手进行 CLI 测试。

---

## 10. CLI 功能验证结果

### 10.1 GRAY 在线切换

MobaXterm 发送：

```text
PROC GRAY
STATUS
```

关闭 MobaXterm 后，运行：

```bash
python tools/pc_dump_rgb565.py --port COM6 --baud 115200 --tag moba_gray_dump
```

测试结果：

```text
2	2026-07-06T21:50:46	moba_gray_dump	captures/005_moba_gray_dump_20260706_215046.png	102.587708	0	10.880208	102.389635	102.874323	102.389635	0.995289	0.995289	1	510.336961
```

分析：

```text
R/G = 0.995289
B/G = 0.995289
B/R = 1
```

说明 RGB 三通道基本一致，灰度模式切换成功。

### 10.2 BINARY 在线切换

MobaXterm 发送：

```text
PROC BINARY
THR 128
STATUS
```

关闭 MobaXterm 后，运行 Python DUMP。

测试结果：

```text
3	2026-07-06T21:52:45	moba_reset_bypass_dump	captures/006_moba_reset_bypass_dump_20260706_215245.png	81.892188	67.885417	32.114583	81.892188	81.892188	81.892188	1	1	1	5425.523438
```

说明：

```text
该行实际为 BINARY 模式测试结果，但 tag 名误写为 moba_reset_bypass_dump。
```

分析：

```text
shadow_ratio = 67.885417%
highlight_ratio = 32.114583%
shadow_ratio + highlight_ratio = 100%
R/G = 1
B/G = 1
B/R = 1
```

说明图像被成功处理为黑白二值图。

### 10.3 RESET 恢复默认 BYPASS

MobaXterm 发送：

```text
RESET
STATUS
```

关闭 MobaXterm 后，运行：

```bash
python tools/pc_dump_rgb565.py --port COM6 --baud 115200 --tag moba_reset_bypass_dump
```

测试结果：

```text
4	2026-07-06T21:54:09	moba_reset_bypass_dump	captures/007_moba_reset_bypass_dump_20260706_215409.png	104.483021	0	10.427083	102.157396	104.365313	110.946927	0.978844	1.063063	1.086039	437.839114
```

分析：

```text
RGB 三通道不再完全相等。
图像恢复为正常彩色输出。
RESET 后 BYPASS 模式恢复成功。
```

---

## 11. 工程内波形文件管理

为了便于后续复盘，将本次调试过程中的波形截图放入工程目录：

```text
docs/images/cli_dump_debug/
```

建议保存以下图片：

```text
normal_dump_uart.png
failed_dump_uart.png
clion_wait_uart_normal.png
clion_pc_1fff_bootloader.png
moba_success_status.png
moba_after_close_dump_success.png
```

对应含义：

```text
normal_dump_uart.png
正常 DUMP 波形：CH2 有 DUMP，CH0 有 OV56RGB5 返回。

failed_dump_uart.png
异常 DUMP 波形：CH2 有 DUMP，CH0 无返回。

clion_wait_uart_normal.png
CLion 正常暂停截图：程序停在 HAL_UART_Receive 等待串口字节。

clion_pc_1fff_bootloader.png
CLion 异常暂停截图：PC 位于 0x1FFFxxxx，说明进入 System Memory Bootloader。

moba_success_status.png
MobaXterm 发送 STATUS 并收到 CLI 响应。

moba_after_close_dump_success.png
关闭 MobaXterm 后，Python DUMP 成功收到图像。
```

Markdown 引用方式：

```markdown
![正常 DUMP UART 波形](docs/images/cli_dump_debug/normal_dump_uart.png)

![异常 DUMP UART 波形](docs/images/cli_dump_debug/failed_dump_uart.png)

![CLion 正常暂停在 HAL_UART_Receive](docs/images/cli_dump_debug/clion_wait_uart_normal.png)

![CLion 暂停在 0x1FFFxxxx Bootloader](docs/images/cli_dump_debug/clion_pc_1fff_bootloader.png)

![MobaXterm STATUS 正常响应](docs/images/cli_dump_debug/moba_success_status.png)

![关闭 MobaXterm 后 Python DUMP 成功](docs/images/cli_dump_debug/moba_after_close_dump_success.png)
```

---

## 12. 上传到 GitHub 的建议操作

在工程根目录创建图片目录：

```bash
mkdir -p docs/images/cli_dump_debug
```

将逻辑分析仪截图、CLion 截图、MobaXterm 测试截图复制到该目录，并按上面的文件名重命名。

然后添加文档和图片：

```bash
git status --short
git add CLI_DUMP_DEBUG_LOG.md docs/images/cli_dump_debug/
git commit -m "docs: add CLI dump debug analysis"
git push
```

如果文档放在 `docs/CLI_DUMP_DEBUG_LOG.md`，则使用：

```bash
git add docs/CLI_DUMP_DEBUG_LOG.md docs/images/cli_dump_debug/
```

---

## 13. 最终结论

本次 DUMP 无响应问题的根因不是固件中的 UART 接收逻辑，也不是 DUMP 协议错误。

最终确认：

```text
旧串口助手打开/关闭 COM 口时，改变了 CH340 的 DTR/RTS 状态。
开发板的一键下载电路利用 DTR/RTS 控制 BOOT0 和 RESET。
DTR/RTS 的变化使 STM32 复位后进入 0x1FFFxxxx System Memory Bootloader。
进入 Bootloader 后，用户固件不再运行。
因此 STM32 不会识别项目自定义的 DUMP 命令，也不会返回 OV56RGB5 图像包。
```

更换为 MobaXterm Serial，并设置：

```text
COM6
115200
Flow control = None
```

后，CLI 与 DUMP 均能正常工作。

本阶段结论：

```text
1. CLI 在线调参功能有效。
2. DUMP 协议保持不变。
3. GRAY 模式切换成功。
4. BINARY 模式切换成功。
5. RESET 恢复 BYPASS 成功。
6. 后续串口 CLI 调试推荐使用 MobaXterm，不再使用会触发一键下载电路的旧串口助手。
```

# Python 串口打开触发 STM32 首次图像请求失败问题排查记录（精简总结）

## 1. 问题背景
- 硬件链路：PC (Python) → COM4/115200 → CH340 USB转串口 → STM32F429 USART1 → UART DMA + StreamBuffer → CameraServiceTask → 二进制图像请求协议 → OV56RGB5 图像帧。
- PC 端通过 `tools/uart_image_request_repeat.py` 连续发送 20 次 14 字节图像请求，每次期望返回 38426 字节图像帧。

## 2. 原始问题现象
- 复现步骤：复位/上电开发板 → 打开正点原子串口助手 → 等待初始化日志输出完 → 不发送 CLI → 关闭串口助手 → 立即运行 Python 脚本。
- 结果：第一次请求固定失败（仅收到 2599 字节，耗时约 8 秒），后续 19 次全部成功（每次 38426 字节）。最终 20 次中成功 19 次，成功率 95%。
- 若复位后不打开串口助手直接运行脚本，则 20 次全部成功。

## 3. 初步怀疑
可能原因包括：UART DMA 首帧异常、StreamBuffer 残留、协议状态机未复位、图像发送中断、串口助手残留日志、Python 打开 COM4 时 DTR/RTS 改变、STM32 意外复位。因缺少失败数据内容，无法定论。

## 4. 保存首次失败数据
- 临时增加失败数据保存功能，失败时保存到 `captures/uart_repeat_first_failed_response.bin`，并打印字节数、前 256 字节文本、前 128 字节十六进制、OV56RGB5 头位置、CRLF 数量。
- 结果：2599 字节，不以 OV56RGB5 开头，未找到该头，CRLF 数量 58。前 256 字节文本为 OV5640 初始化日志（时间戳 729 ms 起）。
- 确认：失败数据不是图像，而是 STM32 复位后的摄像头初始化日志。

## 5. 为什么日志从 729 ms 开始
- Python 脚本打开串口后调用 `ser.reset_input_buffer()`，仅清除调用时已进入 PC 缓冲区的数据。STM32 持续输出日志，早期日志被清空，剩余后半段（从 729 ms 开始）被收到。

## 6. 第一次请求并未触发图像发送
- 第二次成功响应中 `frame_id=1`，说明第一次请求未触发 DUMP，第二次才是首次成功触发。故问题不是图像传输中途损坏，而是第一次请求发送时 STM32 应用尚未准备完成。

## 7. 增加 STM32 复位原因诊断
- 在 `main()` 开始保存 `RCC->CSR`，日志输出复位标志。实测复位原因为 `PIN=1`（外部复位引脚），未出现软件/IWDG/WWDG/POR/BOR 复位。指向板载 USB转串口与自动复位控制线路。

## 8. 分阶段串口诊断
- 依次测试仅打开 COM4、调用 `reset_input_buffer()`、调用 `reset_output_buffer()`、发送请求。早期探针使用 pyserial 默认配置，打印 `DTR=True, RTS=True`，发送请求后收到 0 字节，说明探针本身改变了控制线，不能作为中立工具。

## 9. 根因定位
- pyserial 默认打开串口时 `DTR=True, RTS=True`；开发板 CH340 存在自动下载/复位控制路径。串口助手关闭后控制线保持原状态，Python 打开时切换电平，触发 STM32 外部复位（RCC 显示 PIN=1）。
- 故障链路：关闭串口助手 → 控制线旧状态 → Python 默认打开 COM4（DTR/RTS 变为 True）→ 电平切换 → 板载复位电路影响 NRST → STM32 复位 → 重新初始化 OV5640 并输出日志 → Python 过早发送第一条请求 → CameraServiceTask 未就绪 → 请求被忽略 → Python 收到 2599 字节日志 → 第二次请求时系统已就绪，后续正常。
- 排除了 DMA、StreamBuffer、协议状态机、CRC、任务栈/堆等问题。

## 10. 解决方法
修改 `tools/uart_image_request_repeat.py`，不再直接 `serial.Serial()` 打开，而是先创建未打开对象，设置参数及流控，**在 `ser.open()` 之前**设置 `ser.dtr = False` 和 `ser.rts = False`，并禁用硬件流控 (`rtscts=False, dsrdtr=False`)。使用 `try/finally` 确保关闭。

## 11. 为什么必须在 open() 之前设置
若先 `open()` 再设置 `dtr/rts=False`，打开瞬间仍会经历默认 True → 切换 False 的脉冲，仍可能触发复位。正确顺序：创建对象 → 设置流控禁用 → 设置 dtr/rts=False → 最后 `open()`。

## 12. 单独验证 DTR/RTS 修复
仅修改 DTR/RTS，未增加启动等待、自动重试等额外措施。原有逻辑不变：20 次请求，间隔 0.2 秒，校验头和 CRC，单次失败继续，全部成功才 PASS。

## 13. 三轮硬件复测
每轮按原复现步骤（复位→打开串口助手→等待日志→关闭→立即运行脚本），三轮结果：
- 第1轮：首次请求耗时 3445.9 ms，成功 20 次，失败 0。
- 第2轮：首次请求耗时 3472.4 ms，成功 20 次，失败 0。
- 第3轮：首次请求耗时 3454.9 ms，成功 20 次，失败 0。
  三轮共 60 次请求全部成功，首帧失败为 0，日志混入为 0。

## 14. 修复前后对比
| 项目                               | 修复前                     | 修复后                |
| ---------------------------------- | -------------------------- | --------------------- |
| Python 打开后的 DTR                | True                       | False                 |
| Python 打开后的 RTS                | True                       | False                 |
| 首次请求结果                       | 固定收到 2599 B 初始化日志 | 正常收到 38426 B 图像 |
| 第一次成功 frame_id                | 1，出现在第 2 次请求       | 1，出现在第 1 次请求  |
| 单轮成功率                         | 95%                        | 100%                  |
| 三轮总请求数                       | 未稳定                     | 60                    |
| 三轮失败数                         | 首帧可重复失败             | 0                     |
| 是否增加自动重试/启动等待/修改协议 | 否                         | 否                    |

## 15. 最终结论
根因是 PC 端 pyserial 默认打开串口时 DTR/RTS 为 True，导致控制线切换触发开发板外部复位，STM32 重启，首次请求被忽略，收到日志而非图像。在 `open()` 前固定 DTR/RTS 为 False 后问题稳定解决。

## 16. 后续处理建议
- 所有通过 pyserial 访问 COM4 的工具（如 `uart_image_request_basic.py`、`fault_test.py`、`pc_dump_rgb565.py`）应统一采用相同打开方式。
- 正式修改其他脚本时需单独回归测试。
- 诊断期间临时工具（`uart_open_probe.py` 等）和失败数据文件（`captures/*.bin`）可按需删除，不应提交仓库。