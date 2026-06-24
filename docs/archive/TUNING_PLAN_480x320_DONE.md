# TUNING_PLAN_480x320_DONE.md

> **Status:** Archived
> **Original file:** `TUNING_PLAN.md`
> **Stage:** 480x320 LCD display debugging
>
> This document was used during the 480x320 LCD display debugging stage.
>
> The 480x320 snow/noise issue has been solved by optimizing the FMC/LCD write timing.
>
> Current project focus has moved to PC Dump based image quality analysis and OV5640 AEC/exposure tuning.
>
> **Do not use this document as the current tuning plan.**

------

# Original Tuning Plan: 480x320 LCD Display Stage

## 1. 当前优先级

当前阶段暂不进行 OV5640 画质调参。

本阶段目标是：

```
将真实图像从 320x240 显示，改为铺满 3.5 寸 NT35310 LCD 横屏 480x320 显示。
```

------

## 2. 重要结论

当前工程的显示路径是：

```
OV5640 -> DCMI -> DMA -> LCD GRAM
```

也就是说，当前工程采用的是：

```
DCMI DMA 直接写 LCD GRAM
```

不经过 RAM framebuffer。

因此，如果保持 OV5640 输出 320x240 不变，就不能直接通过软件缩放到 480x320。

原因是：

```
当前工程没有使用 SDRAM framebuffer。
```

所以本阶段的优先方案是：

```
让 OV5640 直接输出 480x320 RGB565，
然后让 DCMI DMA 直接写 LCD 的 480x320 显示窗口。
```

------

## 3. Stage 0：OV5640 直接输出 480x320

### 3.1 目标

本阶段目标如下：

```
1. 保持 DCMI -> LCD 直接显示路径
2. 不使用 SDRAM
3. 不使用 framebuffer
4. 不做软件缩放
5. 新增或调整 OV5640 输出尺寸为 480x320
6. LCD window 改为 480x320
```

### 3.2 要求

本阶段修改要求如下：

```
1. 保留当前稳定的 320x240 QVGA 显示函数
2. 新增 480x320 显示函数或配置
3. 不破坏原来的 QVGA 路径
4. 不修改 SCCB 驱动
5. 不修改 PCF8574 / PWDN / RESET
6. 不做画质调参
7. 不创建 ov5640_tuning.c/.h
```

------

## 4. 禁止修改

本阶段不要修改：

```
1. SCCB 底层驱动
2. PCF8574 / PWDN / RESET
3. GPIO 引脚
4. .ioc 文件
5. SD 卡相关内容
6. 画质调参相关代码
```

------

## 5. 允许少量修改的文件

本阶段允许查看并少量修改：

```
1. OV5640.c
2. OV5640.h
3. main.c
```

如果必须修改 `OV5640cfg.h`，必须说明具体原因，并保留原来的 320x240 配置。

------

## 6. 最终结果

本阶段最终已经完成：

```
1. OV5640 480x320 RGB565 输出成功
2. LCD 480x320 全屏显示成功
3. DCMI + DMA 直接写 LCD GRAM 路径保持不变
4. 雪花问题定位为 FMC/LCD 写时序过慢
5. 最终通过优化 LCD 写时序解决
```

最终采用的 LCD 写时序配置为：

```
#define LCD_MCU_FAST_WRITE_TIMING_ENABLE  1
#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6
```

测试结论：

```
15/15 雪花
10/10 雪花
8/8   正常
6/6   正常，最终采用
4/4   正常
3/3   正常
```

------

## 7. 当前后续方向

480x320 LCD 显示阶段已经结束。

当前工程后续方向为：

```
1. PC Dump 图像导出
2. Python/OpenCV 图像质量分析
3. OV5640 AEC / 曝光调参
4. 后续再进行 AWB、亮度、对比度、饱和度、锐度等画质调试
```