# TEST_LOG.md

## OV5640 480x320 全屏显示调试记录

### 测试目标

当前任务是将 OV5640 摄像头图像铺满 3.5 寸 MCU LCD。

目标显示尺寸：

```text
480x320
```

显示链路为：

```text
OV5640 RGB565 -> DCMI -> DMA -> LCD GRAM
```

当前工程不经过 RAM framebuffer，而是 DCMI DMA 直接写 LCD GRAM。

### 初始问题

原始稳定路径为：

```text
OV5640 320x240 -> DCMI -> DMA -> LCD GRAM
```

该路径可以正常显示真实图像。

尝试扩大到 480x320 时，LCD 出现雪花、滚动或花屏，无法稳定铺满屏幕。

### 分辨率测试记录

| OV5640 输出尺寸 | LCD 显示现象 | 结论            |
| ----------- | -------- | ------------- |
| 320x240     | 正常       | 原稳定路径正常       |
| 400x240     | 雪花       | 宽度超过 320 后不稳定 |
| 480x240     | 雪花       | 宽度超过 320 后不稳定 |
| 320x320     | 正常       | 高度扩展到 320 可行  |
| 400x300     | 雪花       | 宽度超过 320 后不稳定 |
| 480x320     | 雪花       | 初始配置不可用       |

该测试说明问题与“每行输出宽度”高度相关，而不是单纯由总像素数决定。因为 320x320 的总像素数大于 400x240，但 320x320 正常，而 400x240 雪花。

### OV5640 输出尺寸设置排查

初始 480x320 配置只修改：

```text
0x3808 / 0x3809 = output width
0x380A / 0x380B = output height
```

该方法无法稳定实现 480x320。

之后参考正点原子完整摄像头实验工程：

```text
D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\实验38 摄像头实验
```

移植并增加：

```c
uint8_t OV5640_Min_OutSize_Set(uint16_t offx,
                               uint16_t offy,
                               uint16_t width,
                               uint16_t height);
```

该函数设置：

```text
0x3212 = 0x03

0x3808 / 0x3809 = output width
0x380A / 0x380B = output height

0x3810 / 0x3811 = X offset
0x3812 / 0x3813 = Y offset

0x3212 = 0x13
0x3212 = 0xA3
```

之后继续增加：

```c
uint8_t OV5640_Min_ImageWindow_Set(uint16_t offx,
                                   uint16_t offy,
                                   uint16_t width,
                                   uint16_t height);
```

该函数设置：

```text
0x3212 = 0x03

0x3800 / 0x3801 = x start
0x3802 / 0x3803 = y start
0x3804 / 0x3805 = x end
0x3806 / 0x3807 = y end

0x3212 = 0x13
0x3212 = 0xA3
```

但是仅补充 OV5640 输出窗口和输出尺寸设置后，480x320 TestBar 仍然雪花。

因此，问题不只在 OV5640 输出尺寸寄存器。

### 正点原子参考工程对比结论

对比正点原子“实验38 摄像头实验”后发现，OV5640 RGB565 表、DCMI 配置、DMA 配置、LCD GRAM 直写方式基本一致。

关键差异出现在 LCD/FMC 写时序。

参考工程在检测到 LCD ID 为 `0x5310`，即 NT35310 后，会将 FMC 写时序加速：

```text
AddressSetupTime = 3
DataSetupTime    = 3
```

当前工程原先使用较保守写时序：

```text
AddressSetupTime = 15
DataSetupTime    = 15
```

这会导致 DCMI DMA 直接写 LCD GRAM 时，LCD 写入速度不足。尤其在输出宽度超过 320 后，每行 active 数据期间写入压力增大，表现为雪花或花屏。

### FMC 写时序阶梯测试

对 NT35310 LCD 的 FMC 写时序进行阶梯测试，测试结果如下：

| FMC 写时序 | 480x320 TestBar | 480x320 RealImage | 结论              |
| ------- | --------------- | ----------------- | --------------- |
| 15/15   | 雪花              | 雪花                | 写 LCD GRAM 太慢   |
| 10/10   | 雪花              | 雪花                | 写 LCD GRAM 仍然太慢 |
| 8/8     | 正常              | 正常                | 可以稳定显示          |
| 6/6     | 正常              | 正常                | 推荐作为当前默认值       |
| 4/4     | 正常              | 正常                | 可以稳定显示          |
| 3/3     | 正常              | 正常                | 与正点原子参考工程接近     |

最终采用：

```c
#define LCD_MCU_FAST_WRITE_TIMING_ENABLE  1
#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6
```

选择 `6/6` 的原因：

```text
8/8 已经可以正常显示；
6/6 比 8/8 提供更多写入速度余量；
3/3 虽然也正常，但相对更激进；
因此当前选择 6/6 作为稳定默认值。
```

### 最终稳定版本

最终默认模式：

```c
#define CAMERA_MODE CAMERA_MODE_480X320_REAL
```

实际调用路径：

```c
OV5640_Min_InitRGB565_480x320_RealImage();
Camera_DCMI_StartToLCD(0, 0, 480, 320);
```

480x320 TestBar 测试模式：

```c
#define CAMERA_MODE CAMERA_MODE_480X320_TESTBAR
```

320x240 回退模式：

```c
#define CAMERA_MODE CAMERA_MODE_320X240_REAL
```

### 最终结论

当前 OV5640 480x320 全屏显示已经打通。

最终稳定链路为：

```text
OV5640 RGB565 480x320
-> DCMI
-> DMA
-> NT35310 LCD GRAM
-> 480x320 全屏显示
```

最终现象：

```text
480x320 TestBar 正常铺满 LCD
480x320 RealImage 正常铺满 LCD
无雪花
无滚动
无花屏
```

本次问题主因不是 OV5640 输出尺寸寄存器本身，而是 NT35310 LCD 在 MCU/FMC 接口下写 GRAM 速度不足。通过将 FMC 写时序从原来的 15/15 加速到 6/6，解决了 480x320 全屏显示雪花问题。

### 当前未处理问题

当前仅完成 LCD 铺满显示任务。

后续画质问题仍需继续调试，包括：

```text
图像模糊
颜色偏蓝
强光过曝
曝光/增益控制
白平衡
亮度/对比度/饱和度
锐度
对焦
```
