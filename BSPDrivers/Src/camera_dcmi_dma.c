//
// Created by FAKE on 2026/6/2.
//
#include "camera_dcmi_dma.h"
#include "lcd_mcu.h"
#include "bsp_log.h"

DCMI_HandleTypeDef g_camera_dcmi;   // 定义DCMI句柄
DMA_HandleTypeDef  g_camera_dma;    // 定义DCMI使用的DMA句柄

/* ISR 与任务共同访问快照标志；volatile 防止轮询读写被编译器缓存，8 位访问在本平台为原子操作。 */
static volatile uint8_t s_camera_snapshot_active = 0U;
static volatile uint8_t s_camera_snapshot_done = 0U;

/*
 * Apollo V2 + OV5640 fixed DCMI pins:
 * D0=PC6, D1=PC7, D2=PC8, D3=PC9, D4=PC11, D5=PD3, D6=PB8, D7=PB9
 * VSYNC=PB7, HREF=PH8, PCLK=PA6
 */

// 初始化 OV5640 DVP 对应的 DCMI GPIO
void Camera_DCMI_GPIO_Init(void)
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

// 初始化 DCMI 外设及帧中断
void Camera_DCMI_Init(void)
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

// 启动 DCMI 快照，将一帧图像通过 DMA 保存到内存缓冲区
uint8_t Camera_DCMI_StartSnapshotToBuffer(uint32_t buffer_addr, uint32_t word_count)
{
    // 检查缓冲区地址非空、4 字节对齐，且传输数量不为 0
    if ((buffer_addr == 0U) || ((buffer_addr & 0x3U) != 0U) || (word_count == 0U))
    {
        return 1U;
    }

    __HAL_RCC_DMA2_CLK_ENABLE();  // 使能 DMA2 时钟

    g_camera_dma.Instance = DMA2_Stream1;                 // 选择 DMA2 数据流 1
    g_camera_dma.Init.Channel = DMA_CHANNEL_1;            // 选择 DMA 通道 1，对应 DCMI 请求
    g_camera_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;   // 数据方向：外设到内存
    g_camera_dma.Init.PeriphInc = DMA_PINC_DISABLE;       // 外设地址不递增，固定读 DCMI->DR

    /* 关键点：
     * 目标是内存缓冲区，需要依次填充，因此地址必须递增。
     * DCMI->DR 是 32 位寄存器，为了一次性接收两个 RGB565 像素并保证效率，
     * 外设和内存数据宽度都设为 WORD。
     */
    g_camera_dma.Init.MemInc = DMA_MINC_ENABLE;              // 目标地址递增，按字填充缓冲区
    g_camera_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;     // 外设数据宽度：32 位
    g_camera_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;        // 内存数据宽度：32 位
    g_camera_dma.Init.Mode = DMA_NORMAL;                     // DMA 正常模式，传输指定字数后停止
    g_camera_dma.Init.Priority = DMA_PRIORITY_HIGH;          // 设置 DMA 高优先级
    g_camera_dma.Init.FIFOMode = DMA_FIFOMODE_ENABLE;        // 使能 DMA FIFO
    g_camera_dma.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_HALFFULL;  // FIFO 半满触发传输
    g_camera_dma.Init.MemBurst = DMA_MBURST_SINGLE;          // 内存单次突发
    g_camera_dma.Init.PeriphBurst = DMA_PBURST_SINGLE;       // 外设单次突发

    // 先反初始化 DMA，清除可能残留的旧配置
    if (HAL_DMA_DeInit(&g_camera_dma) != HAL_OK)
    {
        return 2U;
    }

    // 初始化 DMA
    if (HAL_DMA_Init(&g_camera_dma) != HAL_OK)
    {
        return 3U;
    }

    __HAL_LINKDMA(&g_camera_dcmi, DMA_Handle, g_camera_dma);  // 把 DMA 句柄绑定到 DCMI 句柄

    // 配置并使能 DMA 传输完成中断，用于通知 CPU 一帧接收完毕
    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 2, 1);   // 设置中断优先级
    HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);           // 使能 DMA 中断

    s_camera_snapshot_done = 0U;      // 复位快照完成标志
    s_camera_snapshot_active = 1U;    // 置位快照进行标志

    /*
     * 启动 DCMI 快照模式，DCMI 会自动打开 DMA 请求。
     * 传输长度为 word_count，DMA 将 DCMI->DR 中的数据逐字搬入 buffer_addr 开始的缓冲区。
     * 当传输达到 word_count 次后，DMA 自动停止并触发完成中断。
     */
    if (HAL_DCMI_Start_DMA(&g_camera_dcmi,
                           DCMI_MODE_SNAPSHOT,
                           buffer_addr,
                           word_count) != HAL_OK)
    {
        s_camera_snapshot_active = 0U;   // 启动失败，清除活动标志
        return 4U;
    }

    return 0U;
}

// 查询 ISR 更新的 DCMI 快照完成标志
uint8_t Camera_DCMI_IsSnapshotDone(void)
{
    return s_camera_snapshot_done;
}

// 清除 DCMI 快照完成标志
void Camera_DCMI_ClearSnapshotDone(void)
{
    s_camera_snapshot_done = 0U;   // 复位标志，表示未完成
}

// 配置 DMA 将 DCMI 数据直接搬运到 LCD GRAM 数据口
void Camera_DCMI_DMA_ConfigToLCD(uint32_t lcd_ram_addr)
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

// 启动 DCMI 连续采集并直接显示到 LCD
void Camera_DCMI_StartToLCD(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    LCD_MCU_SetWindow(x, y, w, h);  // 设置LCD显示窗口
    LCD_MCU_BeginWriteGRAM();       // 发送LCD写GRAM命令

    __HAL_DMA_ENABLE(&g_camera_dma);   // 使能DMA
    __HAL_DCMI_ENABLE(&g_camera_dcmi); // 使能DCMI
    SET_BIT(DCMI->CR, DCMI_CR_CAPTURE); // 启动DCMI捕获
}

// 停止 DCMI 采集，并清除可能残留的快照活动状态
void Camera_DCMI_Stop(void)
{
    CLEAR_BIT(DCMI->CR, DCMI_CR_CAPTURE); // 停止DCMI捕获
    __HAL_DMA_DISABLE(&g_camera_dma);     // 关闭DMA
    // 超时路径也会调用本函数，必须清除活动标志，避免后续帧中断被误判为旧快照完成。
    s_camera_snapshot_active = 0U;
}

// 将 DCMI 中断交给 HAL 处理
void DCMI_IRQHandler(void)
{
    HAL_DCMI_IRQHandler(&g_camera_dcmi);  // 交给HAL库处理中断
}

// 将 DCMI 使用的 DMA2 Stream1 中断交给 HAL 处理
void DMA2_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&g_camera_dma);  // 交给HAL库处理DMA中断
}

// 在帧事件中区分快照完成和连续显示路径
void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)
{
    if (s_camera_snapshot_active != 0U)
    {
        s_camera_snapshot_active = 0U;
        s_camera_snapshot_done = 1U;
        return;
    }

    // LOG_INFO("DCMI frame");  // 调试用：打印收到一帧

    (void)hdcmi;  // 避免未使用参数警告

    __HAL_DCMI_CLEAR_FLAG(&g_camera_dcmi, DCMI_FLAG_FRAMERI);  // 清除帧中断标志

    /*
     * HAL_DCMI_IRQHandler 会清中断使能，这里重新打开。
     */

    __HAL_DCMI_ENABLE_IT(&g_camera_dcmi, DCMI_IT_FRAME);  // 重新使能帧中断
}
