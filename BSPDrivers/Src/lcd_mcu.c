//
// Created by FAKE on 2026/6/2.
//
#include "lcd_mcu.h"
#include "main.h"
#include "lcd_mcu_init.h"
/*
 * 本文件是针对阿波罗 F429 + 3.5寸 MCU TFT 的最小 LCD 配置。
 * 不是正点原子 lcd.c 的复制版。
 * 硬件连接：
 * - FMC_NE1  -> LCD_CS  -> PD7
 * - FMC_A18  -> LCD_RS  -> PD13
 * - FMC_NOE  -> LCD_RD  -> PD4
 * - FMC_NWE  -> LCD_WR  -> PD5
 * - LCD_BL   -> PB5
 * - D0~D15   -> FMC 16-bit data bus
 * 说明：
 * 1. 本文件不是正点原子 lcd.c 的整包复制；
 * 2. 只保留阿波罗 F429 + 3.5寸 MCU LCD 必需的 FMC、NT35310 初始化、窗口、GRAM 写入；
 * 3. NT35310 的初始化寄存器序列参考了正点原子的 0x5310 分支，并压缩为本项目需要的最小可显示配置。
 */

LCD_MCU_Device_t g_lcd_mcu = {0};        // LCD 全局参数结构体，保存 ID、宽高、方向、GRAM 命令等

static SRAM_HandleTypeDef s_lcd_sram;    // FMC SRAM 句柄，LCD 在 STM32 里按外部 SRAM 方式访问

#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6

/* ---------------- low level ---------------- */

void LCD_MCU_WriteReg(uint16_t reg)
{
    reg = reg;                           // 防止编译器过度优化，保持一次真实写操作
    LCD_MCU->REG = reg;                  // 向命令地址写入，RS=0，LCD 认为这是“指令”
}

void LCD_MCU_WriteData(uint16_t data)
{
    data = data;                         // 防止编译器过度优化，保持一次真实写操作
    LCD_MCU->RAM = data;                 // 向数据地址写入，RS=1，LCD 认为这是“数据/像素”
}

void LCD_MCU_WriteRegData(uint16_t reg, uint16_t data)
{
    LCD_MCU->REG = reg;                  // 先写 LCD 指令
    LCD_MCU->RAM = data;                 // 再写该指令对应的数据
}

static uint16_t LCD_MCU_ReadData(void)
{
    volatile uint16_t data;              // volatile 防止读操作被优化掉
    data = LCD_MCU->RAM;                 // 从 LCD 数据口读取数据
    return data;                         // 返回读到的数据
}

/* ---------------- FMC GPIO ---------------- */

static void LCD_MCU_GPIO_FMC_Init(void)
{
    GPIO_InitTypeDef gpio = {0};                         // GPIO 初始化结构体
    FMC_NORSRAM_TimingTypeDef timing_read = {0};         // FMC 读时序配置
    FMC_NORSRAM_TimingTypeDef timing_write = {0};        // FMC 写时序配置

    __HAL_RCC_GPIOD_CLK_ENABLE();                        // 使能 GPIOD 时钟，LCD 控制线和部分数据线在 PD 口
    __HAL_RCC_GPIOE_CLK_ENABLE();                        // 使能 GPIOE 时钟，LCD 部分数据线在 PE 口
    __HAL_RCC_GPIOB_CLK_ENABLE();                        // 使能 GPIOB 时钟，PB5 控制背光
    __HAL_RCC_FMC_CLK_ENABLE();                          // 使能 FMC 外设时钟

    /*
     * FMC pins:
     * PD0  D2
     * PD1  D3
     * PD4  NOE / LCD_RD
     * PD5  NWE / LCD_WR
     * PD7  NE1 / LCD_CS
     * PD8  D13
     * PD9  D14
     * PD10 D15
     * PD13 A18 / LCD_RS
     * PD14 D0
     * PD15 D1
     */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_7 |
               GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_13 |
               GPIO_PIN_14 | GPIO_PIN_15;                // 选择 GPIOD 上所有 LCD/FMC 相关引脚
    gpio.Mode = GPIO_MODE_AF_PP;                          // 复用推挽输出，由 FMC 外设接管这些引脚
    gpio.Pull = GPIO_PULLUP;                              // 上拉，保证空闲状态稳定
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;               // 高速，满足 LCD 并口写入速度
    gpio.Alternate = GPIO_AF12_FMC;                       // 复用功能选择 FMC
    HAL_GPIO_Init(GPIOD, &gpio);                          // 初始化 GPIOD 上的 FMC 引脚

    /*
     * PE7~PE15 -> D4~D12
     */
    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 |
               GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15; // 选择 GPIOE 上的数据线 D4~D12
    HAL_GPIO_Init(GPIOE, &gpio);                          // 初始化 GPIOE 上的 FMC 数据线

    /*
     * PB5 -> LCD backlight
     */
    gpio.Pin = GPIO_PIN_5;                                // PB5 控制 LCD 背光
    gpio.Mode = GPIO_MODE_OUTPUT_PP;                      // 普通推挽输出，不走 FMC
    gpio.Pull = GPIO_PULLUP;                              // 上拉
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;                    // 高速输出
    HAL_GPIO_Init(GPIOB, &gpio);                          // 初始化 PB5
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);   // 拉高 PB5，打开 LCD 背光

    s_lcd_sram.Instance = FMC_NORSRAM_DEVICE;             // 选择 FMC NOR/SRAM 控制器
    s_lcd_sram.Extended = FMC_NORSRAM_EXTENDED_DEVICE;    // 使用扩展时序控制器，读写可分开配置
    s_lcd_sram.Init.NSBank = FMC_NORSRAM_BANK1;           // 使用 FMC Bank1，对应 NE1 片选
    s_lcd_sram.Init.DataAddressMux = FMC_DATA_ADDRESS_MUX_DISABLE; // 地址线和数据线不复用
    s_lcd_sram.Init.MemoryType = FMC_MEMORY_TYPE_SRAM;    // 按 SRAM 类型访问 LCD
    s_lcd_sram.Init.MemoryDataWidth = FMC_NORSRAM_MEM_BUS_WIDTH_16; // 16 位数据总线
    s_lcd_sram.Init.BurstAccessMode = FMC_BURST_ACCESS_MODE_DISABLE; // 不使用突发访问
    s_lcd_sram.Init.WaitSignalPolarity = FMC_WAIT_SIGNAL_POLARITY_LOW; // 等待信号极性，当前不用
    s_lcd_sram.Init.WaitSignalActive = FMC_WAIT_TIMING_BEFORE_WS; // 等待信号时序，当前不用
    s_lcd_sram.Init.WriteOperation = FMC_WRITE_OPERATION_ENABLE; // 允许 FMC 写操作
    s_lcd_sram.Init.WaitSignal = FMC_WAIT_SIGNAL_DISABLE; // 不使用外部等待信号
    s_lcd_sram.Init.ExtendedMode = FMC_EXTENDED_MODE_ENABLE; // 使能扩展模式，读写时序分开
    s_lcd_sram.Init.AsynchronousWait = FMC_ASYNCHRONOUS_WAIT_DISABLE; // 不使用异步等待
    s_lcd_sram.Init.WriteBurst = FMC_WRITE_BURST_DISABLE; // 不使用写突发
    s_lcd_sram.Init.ContinuousClock = FMC_CONTINUOUS_CLOCK_SYNC_ASYNC; // FMC 连续时钟配置，异步设备影响不大
    s_lcd_sram.Init.WriteFifo = 1;                        // 使能写 FIFO，提高写入连续性
    s_lcd_sram.Init.PageSize = FMC_PAGE_SIZE_NONE;        // 不使用页模式

    /*
    * 先用保守时序，保证能稳定读 ID、写 GRAM。
    * FMC 异步访问时序设置
    *
    * 说明：
    * 这些数值单位是 FMC 时钟周期，不是直接的 ns。
    * 若 HCLK = 168 MHz，则 1 个周期约 5.95 ns；
    * 若 HCLK = 180 MHz，则 1 个周期约 5.56 ns；
    *
    * 读 ID 比写 GRAM 慢，所以读时序设置得更保守；
    * 写 LCD 像素需要速度，所以写时序比读时序短。
    */

    /* 读时序：主要用于读取 LCD ID、读取 GRAM，读操作比写操作慢，所以 DataSetupTime 给大一些 */
    timing_read.AddressSetupTime = 15;        // 读地址建立时间：(15+1)周期 ≈ 16*5.56ns = 88.9ns
    timing_read.AddressHoldTime = 0;          // 地址保持时间：异步 SRAM/LCD 模式一般不额外增加
    timing_read.DataSetupTime = 70;           // 读数据建立时间：(70+1)周期 ≈ 71*5.56ns = 394.4ns，读 ID 较慢，给大一些
    timing_read.BusTurnAroundDuration = 0;    // 总线周转时间：当前读写切换不频繁，先不额外增加
    timing_read.CLKDivision = 2;              // 同步模式参数，当前异步 LCD 访问基本不用
    timing_read.DataLatency = 2;              // 同步模式参数，当前异步 LCD 访问基本不用
    timing_read.AccessMode = FMC_ACCESS_MODE_A; // FMC 异步访问模式 A，适合 8080 并口 LCD

    /* 写时序：主要用于写 LCD 命令、写 GRAM 像素，写操作可比读操作快 */
    timing_write.AddressSetupTime = 15;       // 写地址建立时间：(15+1)周期 ≈ 88.9ns，保证 RS/CS/地址线稳定
    timing_write.AddressHoldTime = 0;         // 地址保持时间：异步 LCD 一般不额外增加
    timing_write.DataSetupTime = 15;          // 写数据建立时间：(15+1)周期 ≈ 88.9ns，保证 LCD 能锁存数据
    timing_write.BusTurnAroundDuration = 0;   // 总线周转时间：当前不额外增加
    timing_write.CLKDivision = 2;             // 同步模式参数，当前异步 LCD 访问基本不用
    timing_write.DataLatency = 2;             // 同步模式参数，当前异步 LCD 访问基本不用
    timing_write.AccessMode = FMC_ACCESS_MODE_A; // FMC 异步访问模式 A，适合 MCU LCD 写命令/写数据
    // 若后续要提高刷屏速度，可逐步减小 timing_write.DataSetupTime，但若出现花屏/颜色异常，需要调回更保守值。

    if (HAL_SRAM_Init(&s_lcd_sram, &timing_read, &timing_write) != HAL_OK) // 初始化 FMC 控制器和时序
    {
        Error_Handler();                                  // FMC 初始化失败，进入错误处理
    }

    HAL_Delay(50);                                       // 等待 LCD/FMC 时序稳定
}

static void LCD_MCU_ApplyFastWriteTimingIfNeeded(void)
{
    FMC_NORSRAM_TimingTypeDef timing_write = {0};

    if (g_lcd_mcu.id != LCD_MCU_ID_NT35310)
    {
        return;
    }

    /*
     * NT35310 write timing for DCMI-DMA direct write to LCD GRAM.
     * 15/15 and 10/10 produced snow at 480x320.
     * 8/8, 6/6, 4/4 and 3/3 were verified stable.
     * Use 6/6 as the stable default.
     */
    timing_write.AddressSetupTime = LCD_MCU_WRITE_ADDRESS_SETUP;
    timing_write.AddressHoldTime = 0;
    timing_write.DataSetupTime = LCD_MCU_WRITE_DATA_SETUP;
    timing_write.BusTurnAroundDuration = 0;
    timing_write.CLKDivision = 2;
    timing_write.DataLatency = 2;
    timing_write.AccessMode = FMC_ACCESS_MODE_A;

    (void)FMC_NORSRAM_Extended_Timing_Init(s_lcd_sram.Extended,
                                           &timing_write,
                                           s_lcd_sram.Init.NSBank,
                                           s_lcd_sram.Init.ExtendedMode);
}

/* ---------------- ID ---------------- */

uint16_t LCD_MCU_ReadID(void)
{
    uint16_t id;                                         // 保存最终 LCD ID

    /*
     * NT35310 ID 读取：
     * 0xD4 dummy, 0x01, 0x53, 0x10 -> 0x5310
     */
    LCD_MCU_WriteReg(0xD4);                              // 发送读 ID 指令
    (void)LCD_MCU_ReadData();                            // 第一次假读，LCD 并口读通常需要 dummy read
    (void)LCD_MCU_ReadData();                            // 第二次读到无效/固定字节，跳过
    id = LCD_MCU_ReadData();                             // 读取 ID 高 8 位，应为 0x53
    id <<= 8;                                            // 高 8 位左移，准备拼接低 8 位
    id |= LCD_MCU_ReadData();                            // 读取 ID 低 8 位，应为 0x10，最终 0x5310

    return id;                                           // 返回 LCD ID
}

/* ---------------- direction/window ---------------- */

void LCD_MCU_DisplayDir(uint8_t dir)
{
    g_lcd_mcu.dir = dir ? 1 : 0;                         // 保存当前方向，非 0 都认为是横屏

    /*
     * NT35310 320x480:
     * portrait  : 320x480
     * landscape : 480x320
     *
     * 0x36 为扫描方向寄存器。
     * 若出现镜像/上下颠倒，只需要调整这里的参数。
     */
    LCD_MCU_WriteReg(0x36);                              // 发送内存访问控制指令，设置扫描方向

    if (dir)
    {
        g_lcd_mcu.width = 480;                           // 横屏宽度
        g_lcd_mcu.height = 320;                          // 横屏高度
        LCD_MCU_WriteData(0x28);                         // 写横屏扫描方向参数
    }
    else
    {
        g_lcd_mcu.width = 320;                           // 竖屏宽度
        g_lcd_mcu.height = 480;                          // 竖屏高度
        LCD_MCU_WriteData(0x48);                         // 写竖屏扫描方向参数
    }
}

void LCD_MCU_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x2 = x + w - 1;                              // 计算窗口右边界
    uint16_t y2 = y + h - 1;                              // 计算窗口下边界

    LCD_MCU_WriteReg(g_lcd_mcu.cmd_set_x);                // 发送设置 X 坐标范围指令，通常是 0x2A
    LCD_MCU_WriteData(x >> 8);                            // X 起点高 8 位
    LCD_MCU_WriteData(x & 0xFF);                          // X 起点低 8 位
    LCD_MCU_WriteData(x2 >> 8);                           // X 终点高 8 位
    LCD_MCU_WriteData(x2 & 0xFF);                         // X 终点低 8 位

    LCD_MCU_WriteReg(g_lcd_mcu.cmd_set_y);                // 发送设置 Y 坐标范围指令，通常是 0x2B
    LCD_MCU_WriteData(y >> 8);                            // Y 起点高 8 位
    LCD_MCU_WriteData(y & 0xFF);                          // Y 起点低 8 位
    LCD_MCU_WriteData(y2 >> 8);                           // Y 终点高 8 位
    LCD_MCU_WriteData(y2 & 0xFF);                         // Y 终点低 8 位
}

void LCD_MCU_BeginWriteGRAM(void)
{
    LCD_MCU_WriteReg(g_lcd_mcu.cmd_write_ram);            // 发送写 GRAM 指令，通常是 0x2C，之后写入的都是像素
}

/* ---------------- public init/draw ---------------- */

void LCD_MCU_Init(void)
{
    LCD_MCU_GPIO_FMC_Init();                              // 初始化 FMC GPIO、FMC 控制器、背光
    g_lcd_mcu.cmd_set_x = 0x2A;                           // 设置 0x2A：设置 X 窗口
    g_lcd_mcu.cmd_set_y = 0x2B;                           // 设置 0x2B：设置 Y 窗口
    g_lcd_mcu.cmd_write_ram = 0x2C;                       // 设置 0x2C：开始写 GRAM 像素
    uint16_t id = LCD_MCU_ReadID();                       // 读取 LCD ID

    /*
     * 你已经确认是 0x5310，所以这里即使偶发读 ID 失败，也强制使用 NT35310 初始化。
     * 但是 g_lcd_mcu.id 保留实际读值，方便串口观察。
     */
    g_lcd_mcu.id = id;                                    // 保存读到的 LCD ID

    LCD_MCU_NT35310_RegInit();                            // 执行 NT35310 厂家初始化序列
    LCD_MCU_ApplyFastWriteTimingIfNeeded();               // Apply verified fast write timing for NT35310.
    LCD_MCU_DisplayDir(1);                                // 设置为横屏，480x320

    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_WHITE); // 初始化完成后清白屏
}

void LCD_MCU_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t pixels = (uint32_t)w * (uint32_t)h;           // 计算要写入的像素总数

    LCD_MCU_SetWindow(x, y, w, h);                        // 设置要填充的矩形窗口
    LCD_MCU_BeginWriteGRAM();                             // 发送写 GRAM 指令，准备写像素

    while (pixels--)
    {
        LCD_MCU_WriteData(color);                         // 连续写同一个 RGB565 颜色
    }
}

void LCD_MCU_ShowColorBars(void)
{
    const uint16_t color[8] =
    {
        LCD_COLOR_RED,
        LCD_COLOR_GREEN,
        LCD_COLOR_BLUE,
        LCD_COLOR_CYAN,
        LCD_COLOR_MAGENTA,
        LCD_COLOR_YELLOW,
        LCD_COLOR_WHITE,
        LCD_COLOR_BLACK
    };                                                     // 8 种 RGB565 彩条颜色

    uint16_t bar_w = g_lcd_mcu.width / 8;                  // 每条彩条宽度 = 屏幕宽度 / 8

    for (uint8_t i = 0; i < 8; i++)
    {
        LCD_MCU_Fill(i * bar_w, 0, bar_w, g_lcd_mcu.height, color[i]); // 依次填充每一条彩条
    }
}

/*
 * 更强的 LCD 本地测试：
 * 先全屏红绿蓝白黑，再彩条。
 * 这样可以区分“GRAM 写入失败”和“颜色/窗口错误”。
 */
void LCD_MCU_TestSequence(void)
{
    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_RED);   // 全屏红色
    HAL_Delay(300);                                                         // 保持 300ms 方便观察
    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_GREEN); // 全屏绿色
    HAL_Delay(300);                                                         // 保持 300ms
    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_BLUE);  // 全屏蓝色
    HAL_Delay(300);                                                         // 保持 300ms
    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_WHITE); // 全屏白色
    HAL_Delay(300);                                                         // 保持 300ms
    LCD_MCU_Fill(0, 0, g_lcd_mcu.width, g_lcd_mcu.height, LCD_COLOR_BLACK); // 全屏黑色
    HAL_Delay(300);                                                         // 保持 300ms
    LCD_MCU_ShowColorBars();                                                // 最后显示彩条
}

// /*
//  * 最底层暴力测试：不依赖 Fill 的循环计数和窗口函数之外的任何逻辑。
//  * 如果 LCD_MCU_TestSequence 仍然白屏，可以临时调用：
//  *
//  * LCD_MCU_RawWriteColorForever(LCD_COLOR_RED);
//  *
//  * 只要初始化和 GRAM 写命令有效，屏幕应逐渐/立即变红。
//  */
// void LCD_MCU_RawWriteColorForever(uint16_t color)
// {
//     LCD_MCU_SetWindow(0, 0, g_lcd_mcu.width, g_lcd_mcu.height); // 设置全屏窗口
//     LCD_MCU_BeginWriteGRAM();                                  // 进入连续写像素模式
//
//     while (1)
//     {
//         LCD_MCU_WriteData(color);                              // 无限写同一种颜色，便于排查 GRAM 写入问题
//     }
// }
