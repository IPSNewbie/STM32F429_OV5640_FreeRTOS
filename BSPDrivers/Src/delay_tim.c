//
// Created by FAKE on 2026/5/28.
//
#include "delay_tim.h"
#include "tim.h"

/**
 * @brief  初始化 TIM7 微秒延时计数器
 *
 * @note   本工程中 TIM7 不作为“定时中断”使用，而是作为一个
 *         1 MHz 的自由运行计数器使用。
 *
 *         当前时钟配置：
 *         SYSCLK = 180 MHz
 *         APB1 Prescaler = 4
 *         APB1 Timer Clock = 90 MHz
 *
 *         TIM7 配置：
 *         Prescaler = 89
 *         Counter Period / ARR = 65535
 *
 *         因此 TIM7 的实际计数频率为：
 *         90 MHz / (89 + 1) = 1 MHz
 *
 *         也就是说：
 *         TIM7 每计数 1 次，时间经过 1 us。
 *
 * @note   ARR = 65535 时，TIM7 的完整溢出周期为：
 *         0 ~ 65535，共 65536 个计数
 *         65536 × 1 us = 65536 us = 65.536 ms
 *
 *         但这里并不是等待 TIM7 溢出产生延时，
 *         而是启动 TIM7 后让它一直自由运行。
 *
 *         delay_us() 函数通过读取当前计数值，并计算
 *         “当前计数值 - 起始计数值”的差值来判断是否达到目标延时。
 *
 * @note   本函数需要在 MX_TIM7_Init() 之后调用。
 */
void Delay_TIM7_Init(void)
{
    HAL_TIM_Base_Start(&htim7);
}


/**
 * @brief  微秒级阻塞延时函数
 *
 * @param  us: 需要延时的微秒数
 *
 * @note   本函数基于 TIM7 自由运行计数器实现。
 *
 *         TIM7 已经被配置为 1 MHz 计数频率：
 *         1 个计数 = 1 us
 *
 *         例如：
 *         delay_us(5) 表示等待 TIM7 计数值增加 5，
 *         因此约延时 5 us。
 *
 * @note   这里并不是等待 TIM7 溢出。
 *
 *         虽然 TIM7 的 ARR = 65535，完整溢出周期是 65536 us，
 *         但是 delay_us() 只关心调用函数期间计数器增加了多少。
 *
 *         例如：
 *         start = 1000
 *         调用 delay_us(5)
 *         当 now = 1005 时，
 *         now - start = 5
 *         函数退出。
 *
 * @note   使用 uint16_t 差值计算，可以自动处理 TIM7 溢出。
 *
 *         例如：
 *         start = 65530
 *         delay_us(10)
 *
 *         TIM7 计数过程为：
 *         65530 -> 65531 -> ... -> 65535 -> 0 -> 1 -> 2 -> 3 -> 4
 *
 *         此时 now = 4。
 *
 *         在 uint16_t 无符号运算下：
 *         (uint16_t)(4 - 65530) = 10
 *
 *         因此即使中途发生溢出，也可以正确判断延时时间。
 *
 * @note   由于 TIM7 是 16 位计数器，本函数适合短延时使用。
 *         建议用于 1 us ~ 60000 us 范围内的延时。
 *
 *         对于软件模拟 SCCB / IIC，通常只需要：
 *         2 us、5 us、10 us 这类短延时，因此完全够用。
 *
 * @note   本函数是阻塞式忙等待，会占用 CPU。
 *         适合软件 SCCB、GPIO 短时序等微秒级延时；
 *         不适合毫秒级任务等待。
 *
 *         后续加入 FreeRTOS 后：
 *         微秒级短延时仍可使用本函数；
 *         毫秒级任务延时应使用 vTaskDelay()。
 */
void delay_us(uint16_t us)
{
    uint16_t start;
    uint16_t now;

    start = __HAL_TIM_GET_COUNTER(&htim7);

    while (1)
    {
        now = __HAL_TIM_GET_COUNTER(&htim7);

        if ((uint16_t)(now - start) >= us)
        {
            break;
        }
    }
}