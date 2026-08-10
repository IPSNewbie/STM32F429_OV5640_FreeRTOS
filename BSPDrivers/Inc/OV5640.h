#ifndef ISP_OV5640_OV5640_H
#define ISP_OV5640_OV5640_H

#include "stm32f4xx_hal.h" // 提供 HAL_Delay 所需的 HAL 基础类型和声明
#include <stdint.h>         // 提供寄存器地址、尺寸和返回码的固定宽度类型

/**
 * @file OV5640.h
 * @brief OV5640 RGB565 模式初始化、尺寸设置和寄存器诊断接口
 *
 * 这些接口通过阻塞式 SCCB 访问传感器，供系统初始化或 CameraServiceTask
 * 使用。它们不负责 DCMI/DMA 采集，也不提供 ISR 或多任务并发保护。
 * 彩条模式用于隔离场景、曝光和白平衡因素，先验证传感器到接收端的数据链路；
 * 真实图像模式在相同基础配置上关闭彩条并恢复自动曝光/增益路径。
 */

/** @brief OV5640 芯片 ID，正常通信时应读到 0x5640。 */
#define OV5640_MIN_ID       0x5640U

/**
 * @brief 检查 OV5640 芯片 ID
 * @return 0-ID 为 0x5640，1-ID 读取失败或不匹配
 * @note 依赖 SCCB GPIO 已初始化且传感器已退出 PWDN/RESET 状态。
 */
uint8_t OV5640_Min_CheckID(void);

/**
 * @brief 初始化 OV5640 为 RGB565 QVGA 测试彩条输出
 * @return 0-成功，其他值-对应初始化阶段失败
 */
uint8_t OV5640_Min_InitRGB565_QVGA_TestBar(void);

/**
 * @brief 打开或关闭 OV5640 内部测试彩条
 * @param enable 0-关闭，非 0-打开
 * @return 0-成功，1-寄存器写入失败
 * @note 只控制传感器测试图寄存器，不启动 DCMI 或 DMA。
 */
uint8_t OV5640_Min_EnableTestBar(uint8_t enable);

/**
 * @brief 读回关键配置寄存器并输出诊断日志
 * @return 0-成功，1-任一寄存器读取失败
 * @note 该接口只读取并记录值，不比较期望值，也不修改传感器状态。
 */
uint8_t OV5640_Min_ReadBackDebug(void);

/**
 * @brief 回读 OV5640 时序相关寄存器并输出诊断日志
 * @param tag 调用场景标签，例如 "init" 或 "run"；不可为空
 * @retval 0 全部寄存器读取成功
 * @retval 1 tag 为空或任一寄存器读取失败
 * @note 列表涵盖 PLL、输入窗口、输出尺寸、时序、同步和格式等关键寄存器；
 *       该接口只记录回读值，不负责判断配置是否满足某个期望值。
 */
uint8_t OV5640_Min_ReadBackTimingDebug(const char *tag);

/**
 * @brief 设置 OV5640 缩放后的 DVP 输出尺寸及 ISP 偏移
 * @param offx X 方向 ISP 偏移量，单位像素
 * @param offy Y 方向 ISP 偏移量，单位像素
 * @param width 最终输出宽度，单位像素且不可为 0
 * @param height 最终输出高度，单位像素且不可为 0
 * @retval 0 设置成功
 * @retval 1~11 对应寄存器写入失败
 * @retval 12 width 或 height 为 0
 * @note 0x3212 的三次写入保持项目已验证的分组提交顺序；调用期间不可并发访问传感器。
 */
uint8_t OV5640_Min_OutSize_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);

/**
 * @brief 设置 OV5640 传感器或 ISP 输入裁剪窗口
 * @param offx 输入窗口起始 X 坐标，单位像素
 * @param offy 输入窗口起始 Y 坐标，单位像素
 * @param width 输入窗口宽度，单位像素且不可为 0
 * @param height 输入窗口高度，单位像素且不可为 0
 * @retval 0 设置成功
 * @retval 1~11 对应寄存器写入失败
 * @retval 12 尺寸为 0，或起始坐标加尺寸超出 16 位坐标范围
 * @note 窗口结束坐标等于起始坐标加尺寸减 1。
 *       0x3212 的三次写入保持项目已验证的分组提交顺序。
 */
uint8_t OV5640_Min_ImageWindow_Set(uint16_t offx, uint16_t offy, uint16_t width, uint16_t height);

/**
 * @brief  初始化 OV5640 为 RGB565 QVGA (320x240) 真实图像输出
 *         复用已验证的彩条测试初始化，再关闭彩条并开启自动曝光/增益
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_QVGA_RealImage(void);

/**
 * @brief  初始化 OV5640 为 RGB565 160x120 测试彩条输出
 *         复用 QVGA 彩条初始化，再通过 OV5640_Min_OutSize_Set 缩小输出尺寸
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_160x120_TestBar(void);

/**
 * @brief  初始化 OV5640 为 RGB565 160x120 真实图像输出
 *         复用 160x120 测试彩条初始化，再关闭彩条并开启自动曝光/增益
 * @retval 0:成功, 其他:失败码
 */
uint8_t OV5640_Min_InitRGB565_160x120_RealImage(void);

/**
 * @brief  初始化 OV5640 为 RGB565 480x320 测试彩条输出
 *         完整执行基础配置、RGB565 模式、图像窗口、输出尺寸与测试彩条开启
 * @retval 0:成功, 1:芯片ID检查失败, 2:基础初始化表写入失败,
 *         3:RGB565模式表写入失败, 4:图像窗口设置失败, 5:输出尺寸设置失败,
 *         8:输出格式设置失败, 9:测试彩条开启失败
 */
uint8_t OV5640_Min_InitRGB565_480x320_TestBar(void);

/**
 * @brief  初始化 OV5640 为 RGB565 480x320 真实图像输出
 *         复用测试彩条初始化流程，再关闭彩条并开启自动曝光/自动增益
 * @retval 0:成功, 其他:失败码（具体取决于调用的子函数）
 */
uint8_t OV5640_Min_InitRGB565_480x320_RealImage(void);
#endif // ISP_OV5640_OV5640_H
