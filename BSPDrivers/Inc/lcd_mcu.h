//
// Created by FAKE on 2026/6/2.
//

#ifndef ISP_OV5640_LCD_MCU_H
#define ISP_OV5640_LCD_MCU_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/**
 * Apollo F429 + 正点原子 3.5寸 MCU TFT LCD
 * 已确认 LCD ID = 0x5310，对应 NT35310。
 *
 * 硬件：
 * FMC_NE1 -> LCD_CS
 * FMC_A18 -> LCD_RS
 * 16-bit data bus
 *
 * 驱动的核心思想：
 * STM32 通过 FMC 模拟 8080 并口时序，把 LCD 当成外部 SRAM 一样访问。
 * 写 LCD_MCU->REG 表示写 LCD 命令；
 * 写 LCD_MCU->RAM 表示写 LCD 数据或像素数据。
 */
/** @brief LCD 连接的 FMC 存储区编号。 */
#define LCD_MCU_FMC_NEX             1U
/** @brief 作为 LCD RS 命令/数据选择线的 FMC 地址线编号。 */
#define LCD_MCU_FMC_AX              18U

/** @brief 已验证的 NT35310 控制器 ID。 */
#define LCD_MCU_ID_NT35310          0x5310U

/**
 * LCD_MCU_BASE_ADDR 是 LCD 的 FMC 映射地址。
 *
 * 对当前硬件：
 * FMC_NE1 基地址约为 0x60000000；
 * FMC_A18 用作 LCD_RS，也就是命令/数据选择线；
 *
 * LCD_MCU->REG 对应命令地址；
 * LCD_MCU->RAM 对应数据地址。
 */
#define LCD_MCU_BASE_ADDR     ((uint32_t)((0x60000000UL + (0x4000000UL * (LCD_MCU_FMC_NEX - 1U))) | (((1UL << LCD_MCU_FMC_AX) * 2UL) - 2UL)))

/**
 * @brief LCD 在 FMC 地址空间中的命令与数据端口
 */
typedef struct
{
    volatile uint16_t REG; /**< 命令端口，访问时 RS=0 */
    volatile uint16_t RAM; /**< 数据/GRAM 端口，访问时 RS=1 */
} LCD_MCU_TypeDef;

/** @brief LCD 控制器在 STM32 FMC 地址空间中的映射指针。 */
#define LCD_MCU                     ((LCD_MCU_TypeDef *)LCD_MCU_BASE_ADDR)

/**
 * 常用 RGB565 颜色定义。
 *
 * RGB565 格式：
 * R 占 5 bit；
 * G 占 6 bit；
 * B 占 5 bit。
 */
#define LCD_COLOR_BLACK             0x0000
#define LCD_COLOR_WHITE             0xFFFF
#define LCD_COLOR_RED               0xF800
#define LCD_COLOR_GREEN             0x07E0
#define LCD_COLOR_BLUE              0x001F
#define LCD_COLOR_YELLOW            0xFFE0
#define LCD_COLOR_CYAN              0x07FF
#define LCD_COLOR_MAGENTA           0xF81F

/**
 * @brief LCD 控制器及当前显示方向参数
 */
typedef struct
{
    uint16_t id;            /**< LCD 控制器 ID */
    uint16_t width;         /**< 当前方向下的屏幕宽度 */
    uint16_t height;        /**< 当前方向下的屏幕高度 */
    uint8_t  dir;           /**< 显示方向：0-竖屏，1-横屏 */
    uint16_t cmd_set_x;     /**< 设置 X 窗口的命令 */
    uint16_t cmd_set_y;     /**< 设置 Y 窗口的命令 */
    uint16_t cmd_write_ram; /**< 开始写 GRAM 的命令 */
} LCD_MCU_Device_t;

/**
 * LCD 全局设备参数。
 *
 * 初始化 LCD 后，屏幕 ID、宽度、高度、GRAM 命令等都会保存在这里。
 */
extern LCD_MCU_Device_t g_lcd_mcu;

/**
 * @brief  初始化 MCU 接口 LCD。
 *
 * @note   这个函数会完成：
 *         1. FMC GPIO 初始化；
 *         2. FMC/SRAM 控制器初始化；
 *         3. 读取 LCD ID；
 *         4. 调用 NT35310 厂家初始化序列；
 *         5. 设置默认横屏方向；
 *         6. 清屏。
 *
 * @param  无
 * @retval 无
 */
void LCD_MCU_Init(void);

/**
 * @brief  读取 LCD 控制器 ID。
 *
 * @note   当前 3.5 寸 MCU 屏读到的 ID 应该是 0x5310，
 *         对应 NT35310 控制器。
 *
 * @param  无
 * @retval LCD 控制器 ID
 */
uint16_t LCD_MCU_ReadID(void);

/**
 * @brief  设置 LCD 显示方向。
 *
 * @param  dir 显示方向
 *             0：竖屏，通常为 320 x 480；
 *             1：横屏，通常为 480 x 320。
 *
 * @retval 无
 */
void LCD_MCU_DisplayDir(uint8_t dir);

/**
 * @brief  向 LCD 写命令。
 *
 * @note   本质是向 LCD_MCU->REG 地址写入 16 位命令。
 *         例如设置窗口命令 0x2A、0x2B，写 GRAM 命令 0x2C。
 *
 * @param  reg LCD 命令值
 * @retval 无
 */
void LCD_MCU_WriteReg(uint16_t reg);

/**
 * @brief  向 LCD 写数据。
 *
 * @note   本质是向 LCD_MCU->RAM 地址写入 16 位数据。
 *         可以用于写寄存器参数，也可以用于写 RGB565 像素数据。
 *
 * @param  data 要写入的数据
 * @retval 无
 */
void LCD_MCU_WriteData(uint16_t data);

/**
 * @brief  向 LCD 写一个命令，并紧跟写一个数据。
 *
 * @note   常用于简单寄存器配置，例如：
 *         LCD_MCU_WriteRegData(0x3A, 0x55);
 *         表示设置像素格式为 RGB565。
 *
 * @param  reg  LCD 命令
 * @param  data 命令对应的数据
 * @retval 无
 */
void LCD_MCU_WriteRegData(uint16_t reg, uint16_t data);

/**
 * @brief  设置 LCD 显示窗口。
 *
 * @note   后续写入 GRAM 的像素数据会填充到这个窗口区域。
 *         例如设置窗口为 0,0,320,240 后，
 *         后续写入的像素会显示在左上角 320x240 区域。
 *
 * @param  x 窗口左上角 X 坐标
 * @param  y 窗口左上角 Y 坐标
 * @param  w 窗口宽度
 * @param  h 窗口高度
 * @retval 无
 */
void LCD_MCU_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief  准备向 LCD GRAM 写入像素数据。
 *
 * @note   这个函数会发送写 GRAM 命令，一般是 0x2C。
 *         调用后，连续写 LCD_MCU_WriteData() 就是在写像素点。
 *
 * @param  无
 * @retval 无
 */
void LCD_MCU_BeginWriteGRAM(void);

/**
 * @brief  填充 LCD 指定矩形区域为同一种颜色。
 *
 * @param  x     矩形区域左上角 X 坐标
 * @param  y     矩形区域左上角 Y 坐标
 * @param  w     矩形区域宽度
 * @param  h     矩形区域高度
 * @param  color RGB565 颜色值
 *
 * @retval 无
 */
void LCD_MCU_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief  显示 8 色彩条。
 *
 * @note   用于验证 LCD 初始化、FMC 写数据、GRAM 写入是否正常。
 *         如果这个函数可以正常显示彩条，说明 LCD 基础显示链路已经通了。
 *
 * @param  无
 * @retval 无
 */
void LCD_MCU_ShowColorBars(void);

/**
 * @brief  LCD 本地测试序列。
 *
 * @note   该函数会依次显示：
 *         红屏 -> 绿屏 -> 蓝屏 -> 白屏 -> 黑屏 -> 彩条。
 *
 *         用于判断 LCD 是否真正能够被 MCU 写入像素数据。
 *
 * @param  无
 * @retval 无
 */
void LCD_MCU_TestSequence(void);

/**
 * @brief  最底层 LCD 写 GRAM 测试函数。
 *
 * @note   该函数会不断向 LCD GRAM 写入同一种颜色。
 *         主要用于排查 LCD_MCU_Fill() 或窗口设置是否有问题。
 *
 *         一般调试时可以这样用：
 *         LCD_MCU_RawWriteColorForever(LCD_COLOR_RED);
 *
 *         如果屏幕变红，说明 LCD GRAM 写入是通的。
 *         如果仍然白屏，说明 FMC 写数据或 LCD 初始化还有问题。
 *
 * @param  color RGB565 颜色值
 * @retval 无
 */
void LCD_MCU_RawWriteColorForever(uint16_t color);

/**
 * @brief  获取 LCD GRAM 数据地址。
 *
 * @note   这个函数主要给 DCMI + DMA 使用。
 *         在摄像头显示链路中，DMA 会把 DCMI 接收到的 RGB565 数据
 *         直接搬运到 LCD_MCU->RAM，也就是 LCD GRAM 数据口。
 *
 * @param  无
 * @retval LCD GRAM 数据寄存器地址
 */
static inline volatile uint16_t *LCD_MCU_GetRAMAddress(void)
{
    return &LCD_MCU->RAM;
}

#endif // ISP_OV5640_LCD_MCU_H
