//
// Created by FAKE on 2026/6/2.
//

#ifndef ISP_OV5640_CAMERA_DCMI_DMA_H
#define ISP_OV5640_CAMERA_DCMI_DMA_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 摄像头当前测试输出尺寸：QVGA = 320 x 240 */
#define CAMERA_QVGA_WIDTH       320U
#define CAMERA_QVGA_HEIGHT      240U

/* DCMI 句柄：管理 DCMI 外设的初始化参数、状态和寄存器操作 */
extern DCMI_HandleTypeDef g_camera_dcmi;

/* DMA 句柄：管理 DCMI 数据搬运使用的 DMA2_Stream1 */
extern DMA_HandleTypeDef  g_camera_dma;

/* 初始化 DCMI 相关 GPIO 引脚
 * 包括 D0~D7、PCLK、VSYNC、HREF
 * 这些引脚需要配置为 DCMI 复用功能 AF13
 */
void Camera_DCMI_GPIO_Init(void);

/* 初始化 DCMI 外设
 * 配置硬件同步模式、PCLK 采样边沿、VSYNC/HREF 极性、
 * 8 位数据宽度、帧中断和 NVIC
 */
void Camera_DCMI_Init(void);

/* Capture exactly one frame from DCMI into a word-aligned SRAM buffer. */
uint8_t Camera_DCMI_StartSnapshotToBuffer(uint32_t buffer_addr, uint32_t word_count);
uint8_t Camera_DCMI_IsSnapshotDone(void);
void Camera_DCMI_ClearSnapshotDone(void);

/* 配置 DMA，让 DCMI 接收到的数据直接写入 LCD GRAM
 * lcd_ram_addr 是 LCD 数据口地址，例如：
 * (uint32_t)LCD_MCU_GetRAMAddress()
 */
void Camera_DCMI_DMA_ConfigToLCD(uint32_t lcd_ram_addr);

/* 启动 DCMI + DMA 显示链路
 * 先设置 LCD 显示窗口，再进入 LCD GRAM 写模式，
 * 最后启动 DCMI 捕获
 *
 * x, y：图像显示在 LCD 上的起始坐标
 * w, h：显示窗口宽度和高度，例如 320 x 240
 */
void Camera_DCMI_StartToLCD(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* 停止 DCMI 捕获和 DMA 传输
 * 用于暂停摄像头显示，或者重新配置 OV5640 / DCMI / LCD 窗口前调用
 */
void Camera_DCMI_Stop(void);

#endif // ISP_OV5640_CAMERA_DCMI_DMA_H
