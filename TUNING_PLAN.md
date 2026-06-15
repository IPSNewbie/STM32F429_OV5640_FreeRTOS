# TUNING_PLAN.md

## 当前优先级

当前先不做 OV5640 调参。

第一优先级是：
把当前 OV5640 RGB565 QVGA 320x240 真实图像放大到 3.5 寸 NT35310 LCD 横屏 480x320 显示。

## 重要结论

当前工程的摄像头显示路径是：

OV5640 -> DCMI -> DMA -> LCD GRAM

也就是 DCMI DMA 直接写 LCD，不经过 RAM 帧缓冲。

这种路径不能直接做 320x240 到 480x320 的缩放。

因此，全屏缩放需要新增一条路径：

OV5640 -> DCMI -> DMA -> RAM framebuffer -> LCD 缩放显示

## Stage 0-A：检查帧缓冲可行性

先检查：
- 工程是否已经初始化外部 SDRAM
- 是否有 SDRAM 地址宏
- 是否有 linker script / memory map
- 当前 DCMI DMA 数据宽度和对齐方式
- 当前 LCD 写 RGB565 的方式

不要修改代码。

## Stage 0-B：新增 framebuffer 采集路径

允许新增 DCMI 到 RAM 的采集接口。

要求：
- 保留原来的 Camera_DCMI_DMA_ConfigToLCD()
- 保留原来的 Camera_DCMI_StartToLCD()
- 不破坏当前能显示 320x240 真实图像的稳定路径
- 只新增 DCMI 到 framebuffer 的并行路径

## Stage 0-C：新增 LCD 缩放显示函数

新增 LCD 层函数，把 320x240 RGB565 framebuffer 显示到 480x320 LCD。

显示模式：
- NATIVE：320x240 原图居中
- FIT：等比例放大，有黑边，不变形
- FILL：等比例放大，裁剪上下，铺满屏幕，不变形
- STRETCH：直接拉伸，铺满但可能变形

默认先测试：
1. NATIVE
2. STRETCH
3. FILL

## 禁止修改

不要修改：
- OV5640 初始化表
- SCCB 驱动
- PCF8574 / PWDN / RESET
- OV5640 输出分辨率
- 画质调参功能

暂时不要创建：
- ov5640_tuning.c
- ov5640_tuning.h