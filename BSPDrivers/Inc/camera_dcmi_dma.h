//
// Created by FAKE on 2026/6/2.
//

#ifndef ISP_OV5640_CAMERA_DCMI_DMA_H
#define ISP_OV5640_CAMERA_DCMI_DMA_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/** @brief DCMI 外设句柄，供中断、快照控制和诊断代码共享。 */
extern DCMI_HandleTypeDef g_camera_dcmi;

/** @brief DCMI 数据搬运使用的 DMA2_Stream1 句柄。 */
extern DMA_HandleTypeDef  g_camera_dma;

/**
 * @brief 初始化 DCMI 的 D0~D7、PCLK、VSYNC 和 HREF 引脚
 * @note 将相关引脚配置为 DCMI 复用功能 AF13。
 */
void Camera_DCMI_GPIO_Init(void);

/**
 * @brief 初始化 DCMI 外设及其中断
 * @note 保持当前硬件同步、采样边沿、极性和 8 位数据宽度配置。
 */
void Camera_DCMI_Init(void);

/**
 * @brief 启动 DCMI 快照，将一帧图像通过 DMA 保存到内存
 * @param buffer_addr 目标缓冲区地址，必须按 4 字节对齐
 * @param word_count 要传输的 32 位字数量
 * @retval 0 启动成功
 * @retval 1 参数非法
 * @retval 2 DMA 反初始化失败
 * @retval 3 DMA 初始化失败
 * @retval 4 DCMI DMA 启动失败
 */
uint8_t Camera_DCMI_StartSnapshotToBuffer(uint32_t buffer_addr, uint32_t word_count);

/**
 * @brief 查询 DCMI 快照是否传输完成
 * @note Stage 15C 的同步采集由 Task Notification 唤醒；该标志保留底层兼容性。
 * @retval 0 尚未完成
 * @retval 1 DMA 传输完成回调已确认本次快照完成
 */
uint8_t Camera_DCMI_IsSnapshotDone(void);

/**
 * @brief 清除快照完成标志
 * @note CaptureTask 在启动下一次快照前调用，避免保留上一次完成状态。
 */
void Camera_DCMI_ClearSnapshotDone(void);

/**
 * @brief 配置 DMA，使 DCMI 数据直接写入 LCD GRAM
 * @param lcd_ram_addr LCD 数据口地址，例如 LCD_MCU_GetRAMAddress() 的转换值
 * @note 该接口属于已验证的实时 LCD 显示链路，不用于帧缓冲快照。
 */
void Camera_DCMI_DMA_ConfigToLCD(uint32_t lcd_ram_addr);

/**
 * @brief 启动 DCMI 到 LCD GRAM 的直接 DMA 显示链路
 * @param x LCD 窗口左上角 X 坐标
 * @param y LCD 窗口左上角 Y 坐标
 * @param w LCD 显示窗口宽度
 * @param h LCD 显示窗口高度
 * @note 先设置 LCD 窗口并进入 GRAM 写模式，再启动 DCMI 捕获。
 */
void Camera_DCMI_StartToLCD(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief 停止 DCMI 捕获和 DMA 传输
 * @note 用于暂停摄像头链路或重新配置 OV5640、DCMI、LCD 窗口。
 */
void Camera_DCMI_Stop(void);

#endif // ISP_OV5640_CAMERA_DCMI_DMA_H
