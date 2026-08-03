# Python串口DTR/RTS触发STM32首帧请求失败问题排查记录

## 1. 问题背景

本项目使用STM32F429、OV5640、USART1和CH340，PC端通过COM4、115200波特率及Python pyserial发送14 B二进制图像请求，STM32返回38426 B的OV56RGB5图像响应。

## 2. 原始现象

固定复现路径如下：

1. 开发板复位或重新上电。
2. 打开正点原子串口助手并等待初始化日志结束。
3. 关闭串口助手。
4. 立即运行连续图像请求脚本。
5. 第一次请求收到2599 B并失败，后续19次请求全部成功。
6. 第一次成功图像的`frame_id`为1。

## 3. 失败数据分析

- 失败响应保存路径：`captures/uart_repeat_first_failed_response.bin`。
- 保存长度：2599 B。
- CRLF数量：58。
- 数据中未找到`OV56RGB5`。
- 内容是OV5640初始化日志，不是残缺图像，也不是图像payload CRC错误。

## 4. 排除的原因

结合失败数据和后续请求恢复情况，已经排除：

- UART DMA持续运行失败。
- StreamBuffer溢出。
- 二进制状态机持续故障。
- 图像发送中途丢失。
- FreeRTOS任务栈不足。
- FreeRTOS Heap不足。

## 5. RCC复位诊断

真实板测记录：

```text
RESET_CAUSE_READY raw=0x04000000 PIN=1 POR=0 BOR=0 SOFTWARE=0 IWDG=0 WWDG=0 LOW_POWER=0
```

该结果表明`PINRSTF`有效，未检测到软件复位、IWDG复位或WWDG复位，当前证据指向外部复位路径。RCC复位标志在其他场景中可能同时出现多个，后续判断仍应保留原始CSR值和所有标志。

## 6. pyserial默认控制线

旧探针观察到：

```text
DTR=True
RTS=True
```

直接调用`serial.Serial(port=...)`会立即打开串口。在显式配置控制线之前，pyserial默认DTR/RTS状态可能产生电平切换；板载CH340自动下载或复位线路可能将该变化传递到NRST路径。

## 7. 根因链路

```text
正点原子串口助手关闭
→ Python默认方式打开COM4
→ DTR和RTS切换为True
→ 外部复位路径被触发
→ STM32重新启动
→ OV5640输出初始化日志
→ 第一条请求发送过早并被忽略
→ Python收到2599 B日志
→ 第二条请求时系统已经准备完成
```

该链路说明特定Python默认打开方式可能触发复位，不能表述为“打开串口助手必然导致复位”。

## 8. 修复方式

正确打开顺序如下：

```python
ser = serial.Serial()
ser.port = port_name
ser.baudrate = baudrate
ser.timeout = timeout_seconds
ser.write_timeout = 2.0
ser.rtscts = False
ser.dsrdtr = False
ser.dtr = False
ser.rts = False
ser.open()
```

四项控制线及流控配置必须全部位于`open()`之前，并在异常路径通过`finally`关闭已打开的串口。

## 9. 为什么不能open后再设置

`open()`瞬间可能先应用pyserial或驱动默认控制线状态。打开后再设置为False，仍可能已经产生一次电平变化并触发板载外部复位路径，因此“先配置、后打开”的顺序是修复关键。

## 10. 修复时未增加的功能

本次修复没有增加：

- 启动等待。
- 自动重试。
- 串口静默检测。
- 帧头重新同步。
- 请求重发。
- STM32协议修改。

这样可以单独验证DTR/RTS控制线配置是否为首帧失败的主要原因。

## 11. 三轮复测结果

第1轮：

```text
DTR=False
RTS=False
第一次请求耗时3445.9 ms
成功20
失败0
PASS
```

第2轮：

```text
DTR=False
RTS=False
第一次请求耗时3472.4 ms
成功20
失败0
PASS
```

第3轮：

```text
DTR=False
RTS=False
第一次请求耗时3454.9 ms
成功20
失败0
PASS
```

汇总：总请求60次，成功60次，失败0次，首帧失败0次，初始化日志混入0次。

## 12. 正式工具统一

本轮统一检查以下正式工具：

- `tools/uart_image_request_basic.py`：改为open前配置DTR/RTS和流控。
- `tools/uart_image_request_repeat.py`：保持已经完成三轮硬件验证的配置不变。
- `tools/uart_image_request_fault_test.py`：改为open前配置DTR/RTS和流控。
- `tools/pc_dump_rgb565.py`：在专用串口打开函数中统一配置，不再在open后切换控制线。

## 13. 临时诊断文件

- `tools/uart_open_probe.py`
- `tools/uart_open_probe_no_clear.py`
- `tools/uart_operation_probe.py`
- `tools/uart_first_write_probe.py`

这些文件仅用于故障定位，不作为正式功能工具，也不应混入正式回归测试入口。

Stage 10 Round 1收尾时已删除以上四个临时诊断脚本，只保留正式工具。

## 14. 四个正式工具回归结果

- `tools/uart_image_request_basic.py`：`DTR=False`，`RTS=False`，PASS。
- `tools/uart_image_request_fault_test.py`：`DTR=False`，`RTS=False`，PASS。
- `tools/pc_dump_rgb565.py`：`DTR=False`，`RTS=False`，Frame 9，Header与payload CRC校验通过。
- `tools/uart_image_request_repeat.py`：`DTR=False`，`RTS=False`，20/20 PASS。

## 15. 结论

- 根因位于PC端串口控制线配置，而不是STM32协议稳定性。
- 在`open()`前设置`DTR=False`和`RTS=False`后，首帧失败问题稳定消失。
- 本次正式工具统一没有改变协议、等待、重试或图像处理逻辑。
- 后续新增pyserial工具必须遵守“先关闭硬件流控并设置DTR/RTS，再打开串口”的顺序。
