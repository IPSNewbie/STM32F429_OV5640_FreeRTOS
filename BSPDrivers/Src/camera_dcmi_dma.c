//
// Created by FAKE on 2026/6/2.
//
#include "camera_dcmi_dma.h"
#include "lcd_mcu.h"
#include "bsp_log.h"

DCMI_HandleTypeDef g_camera_dcmi;   // 定义DCMI句柄
DMA_HandleTypeDef  g_camera_dma;    // 定义DCMI使用的DMA句柄

/*
 * Apollo V2 + OV5640 fixed DCMI pins:
 * D0=PC6, D1=PC7, D2=PC8, D3=PC9, D4=PC11, D5=PD3, D6=PB8, D7=PB9
 * VSYNC=PB7, HREF=PH8, PCLK=PA6
 */

void Camera_DCMI_GPIO_Init(void)    // 初始化DCMI相关GPIO
{
    GPIO_InitTypeDef gpio = {0};    // 定义GPIO初始化结构体并清零

    __HAL_RCC_DCMI_CLK_ENABLE();    // 使能DCMI外设时钟
    __HAL_RCC_GPIOA_CLK_ENABLE();   // 使能GPIOA时钟
    __HAL_RCC_GPIOB_CLK_ENABLE();   // 使能GPIOB时钟
    __HAL_RCC_GPIOC_CLK_ENABLE();   // 使能GPIOC时钟
    __HAL_RCC_GPIOD_CLK_ENABLE();   // 使能GPIOD时钟
    __HAL_RCC_GPIOH_CLK_ENABLE();   // 使能GPIOH时钟

    gpio.Mode = GPIO_MODE_AF_PP;              // 设置为复用推挽输出，实际为输入，复用功能模式下，具体方向输入、输出方向由外设决定
    gpio.Pull = GPIO_PULLUP;                  // 设置上拉
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;   // 设置高速
    gpio.Alternate = GPIO_AF13_DCMI;          // 复用为DCMI功能

    gpio.Pin = GPIO_PIN_6;        // 选择PA6
    HAL_GPIO_Init(GPIOA, &gpio);  // 初始化PA6为DCMI_PCLK

    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;  // 选择PB7/PB8/PB9
    HAL_GPIO_Init(GPIOB, &gpio);                      // 初始化PB7为VSYNC，PB8/PB9为D6/D7

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_11;  // 选择PC6/7/8/9/11
    HAL_GPIO_Init(GPIOC, &gpio);                                                // 初始化PC6~PC11相关引脚为D0~D4

    gpio.Pin = GPIO_PIN_3;        // 选择PD3
    HAL_GPIO_Init(GPIOD, &gpio);  // 初始化PD3为D5

    gpio.Pin = GPIO_PIN_8;        // 选择PH8
    HAL_GPIO_Init(GPIOH, &gpio);  // 初始化PH8为HREF/HSYNC
}

void Camera_DCMI_Init(void)        // 初始化DCMI外设
{
    Camera_DCMI_GPIO_Init();       // 先初始化DCMI GPIO引脚

    g_camera_dcmi.Instance = DCMI;                              // 选择DCMI外设
    g_camera_dcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;     // 使用硬件同步VSYNC/HREF
    g_camera_dcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;   // PCLK上升沿采样数据
    g_camera_dcmi.Init.VSPolarity = DCMI_VSPOLARITY_LOW;        // VSYNC低电平有效
    g_camera_dcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;        // HREF/HSYNC低电平有效
    g_camera_dcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;         // 采集所有帧
    g_camera_dcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;  // 使用8位数据总线
    g_camera_dcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;            // 禁用JPEG模式，采集RGB565数据

    HAL_DCMI_Init(&g_camera_dcmi);    // 调用HAL库初始化DCMI

    __HAL_DCMI_ENABLE_IT(&g_camera_dcmi, DCMI_IT_FRAME);   // 使能帧中断
    __HAL_DCMI_DISABLE_IT(&g_camera_dcmi,                  // 关闭其他DCMI中断
                          DCMI_IT_LINE |                  // 关闭行中断
                          DCMI_IT_VSYNC |                 // 关闭VSYNC中断，DCMI_IT_FRAME 和 DCMI_IT_VSYNC 的区别：DCMI_IT_FRAME：帧完成中断，DCMI_IT_VSYNC： VSYNC 事件中断。
                          DCMI_IT_ERR |                   // 关闭同步错误中断
                          DCMI_IT_OVR);                   // 关闭溢出中断

    HAL_NVIC_SetPriority(DCMI_IRQn, 2, 2);  // 设置DCMI中断优先级
    HAL_NVIC_EnableIRQ(DCMI_IRQn);          // 使能DCMI中断
}

void Camera_DCMI_DMA_ConfigToLCD(uint32_t lcd_ram_addr)  // 配置DMA把DCMI数据搬到LCD
{
    __HAL_RCC_DMA2_CLK_ENABLE();  // 使能DMA2时钟

    g_camera_dma.Instance = DMA2_Stream1;                 // 选择DMA2数据流1
    g_camera_dma.Init.Channel = DMA_CHANNEL_1;            // 选择DMA通道1，对应DCMI请求
    g_camera_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;   // 数据方向：外设到内存
    g_camera_dma.Init.PeriphInc = DMA_PINC_DISABLE;       // 外设地址不递增，固定读DCMI->DR

    /* 关键点：
     * 目标是 LCD GRAM 数据口，地址不能递增。
     * DCMI->DR 每次给出 32bit，包含两个 RGB565 像素。
     * LCD RAM 是 16bit 外设口，因此目标数据宽度用 HALFWORD。
     */

    g_camera_dma.Init.MemInc = DMA_MINC_DISABLE;              // 目标地址不递增，固定写LCD RAM口
    g_camera_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;     // DCMI数据寄存器按32位读取
    g_camera_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;    // LCD RAM按16位写入
    g_camera_dma.Init.Mode = DMA_CIRCULAR;                   // 使用DMA循环模式
    g_camera_dma.Init.Priority = DMA_PRIORITY_HIGH;          // 设置DMA高优先级
    g_camera_dma.Init.FIFOMode = DMA_FIFOMODE_ENABLE;        // 使能DMA FIFO
    g_camera_dma.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_HALFFULL;  // FIFO半满触发
    g_camera_dma.Init.MemBurst = DMA_MBURST_SINGLE;          // 内存单次突发
    g_camera_dma.Init.PeriphBurst = DMA_PBURST_SINGLE;       // 外设单次突发

    HAL_DMA_DeInit(&g_camera_dma);   // 先反初始化DMA，清除旧配置
    HAL_DMA_Init(&g_camera_dma);     // 初始化DMA

    __HAL_LINKDMA(&g_camera_dcmi, DMA_Handle, g_camera_dma);  // 把DMA句柄绑定到DCMI句柄

    /*
     * 长度 = 1，目标地址不递增，循环模式。
     * 每个 DMA request 都把 DCMI->DR 写到同一个 LCD RAM 数据口。
     */

    __HAL_UNLOCK(&g_camera_dma);  // 解锁DMA句柄，避免HAL_BUSY
    HAL_DMA_Start(&g_camera_dma,  // 启动DMA
                  (uint32_t)&DCMI->DR,  // 源地址：DCMI数据寄存器
                  lcd_ram_addr,         // 目标地址：LCD GRAM数据口
                  1);                   // 传输长度，即传输计数器为1个单位
}

void Camera_DCMI_StartToLCD(uint16_t x, uint16_t y, uint16_t w, uint16_t h)  // 启动DCMI采集并显示到LCD
{
    LCD_MCU_SetWindow(x, y, w, h);  // 设置LCD显示窗口
    LCD_MCU_BeginWriteGRAM();       // 发送LCD写GRAM命令

    __HAL_DMA_ENABLE(&g_camera_dma);   // 使能DMA
    __HAL_DCMI_ENABLE(&g_camera_dcmi); // 使能DCMI
    SET_BIT(DCMI->CR, DCMI_CR_CAPTURE); // 启动DCMI捕获
}

void Camera_DCMI_Stop(void)  // 停止DCMI采集
{
    CLEAR_BIT(DCMI->CR, DCMI_CR_CAPTURE); // 停止DCMI捕获
    __HAL_DMA_DISABLE(&g_camera_dma);     // 关闭DMA
}

void DCMI_IRQHandler(void)  // DCMI中断服务函数
{
    HAL_DCMI_IRQHandler(&g_camera_dcmi);  // 交给HAL库处理中断
}

void DMA2_Stream1_IRQHandler(void)  // DMA2_Stream1中断服务函数
{
    HAL_DMA_IRQHandler(&g_camera_dma);  // 交给HAL库处理DMA中断
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)  // DCMI帧中断回调
{
    // LOG_INFO("DCMI frame");  // 调试用：打印收到一帧

    (void)hdcmi;  // 避免未使用参数警告

    __HAL_DCMI_CLEAR_FLAG(&g_camera_dcmi, DCMI_FLAG_FRAMERI);  // 清除帧中断标志

    /*
     * HAL_DCMI_IRQHandler 会清中断使能，这里重新打开。
     */

    __HAL_DCMI_ENABLE_IT(&g_camera_dcmi, DCMI_IT_FRAME);  // 重新使能帧中断
}