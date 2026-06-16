/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bsp_log.h"
#include "bsp_PCF8574.h"
#include "delay_tim.h"
#include "bsp_sccb.h"
#include "lcd_mcu.h"
#include "camera_dcmi_dma.h"
#include "OV5640.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CAMERA_MODE_480X320_REAL       0
#define CAMERA_MODE_480X320_TESTBAR    1
#define CAMERA_MODE_320X240_REAL       2

#define CAMERA_MODE                    CAMERA_MODE_480X320_REAL

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C2_Init();
  MX_USART1_UART_Init();
  MX_TIM7_Init();
  /* USER CODE BEGIN 2 */
  log_init(&huart1);
  log_set_level(LOG_LEVEL_DEBUG);   // 输出 DEBUG 及以上等级
  Delay_TIM7_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  if (PCF8574_Init())
  {
    LOG_ERROR("PCF8574 initialization failed, check I2C connection");
    HAL_Delay(100);
  }
  else
  {
    LOG_INFO("PCF8574 initialized successfully");
  }

  /* 1. 先退出掉电模式：PWDN = 0 */
  // MyI2C_Init();在MX_I2C2_Init完成了硬件IIC初始化，用于PCF8574通信
  PCF8574_WriteBit(PCF8574_OV_PWDN_IO, 0);
  HAL_Delay(20);

  /* 2. 再硬复位 OV5640 */
  HAL_GPIO_WritePin(OV_RESET_GPIO_Port, OV_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(20);

  HAL_GPIO_WritePin(OV_RESET_GPIO_Port, OV_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(50);

  /* 3. 初始化软件 SCCB GPIO */
  //MX_GPIO_Init();在MX_GPIO_Init完成了SCCB GPIO的初始化，OV_SDA和OV_SCL已经配置为开漏输出，并上拉

  /* 4. 读取 ID */
  uint16_t ov_id = OV5640_ReadID();

  if (ov_id == 0x5640)
  {
    LOG_INFO("OV5640 OK, ID = 0x%04X", ov_id);
  }
  else
  {
    LOG_ERROR("OV5640 ERROR, ID = 0x%04X", ov_id);
  }

  /*
    * 5. LCD 初始化，本地彩条验证
    */
  LCD_MCU_Init();
  //关闭LCD彩条测试
  // LCD_MCU_TestSequence();
  HAL_Delay(1000);

  /*
   * 6. Configure OV5640 output mode.
   */
#if CAMERA_MODE == CAMERA_MODE_480X320_REAL
  uint8_t ret = OV5640_Min_InitRGB565_480x320_RealImage();
  LOG_INFO("OV5640 480x320 real image init ret = %d", ret);
#elif CAMERA_MODE == CAMERA_MODE_480X320_TESTBAR
  uint8_t ret = OV5640_Min_InitRGB565_480x320_TestBar();
  LOG_INFO("OV5640 480x320 testbar init ret = %d", ret);
#elif CAMERA_MODE == CAMERA_MODE_320X240_REAL
  uint8_t ret = OV5640_Min_InitRGB565_QVGA_RealImage();
  LOG_INFO("OV5640 320x240 real image init ret = %d", ret);
#else
#error "Unsupported CAMERA_MODE"
#endif
  /*
   * 7. 初始化 DCMI
   */
  Camera_DCMI_Init();

  /*
   * 8. 配置 DMA：DCMI 数据直接写 LCD GRAM
   */
  Camera_DCMI_DMA_ConfigToLCD((uint32_t)LCD_MCU_GetRAMAddress());

  /*
   * 9. Set LCD window and start DCMI capture.
   */
#if (CAMERA_MODE == CAMERA_MODE_480X320_REAL) || (CAMERA_MODE == CAMERA_MODE_480X320_TESTBAR)
  Camera_DCMI_StartToLCD(0, 0, 480, 320);
#elif CAMERA_MODE == CAMERA_MODE_320X240_REAL
  Camera_DCMI_StartToLCD(0, 0, 320, 240);
#else
#error "Unsupported CAMERA_MODE"
#endif
  HAL_Delay(100);

  LOG_INFO("DCMI CR   = 0x%08lX", DCMI->CR);
  LOG_INFO("DCMI SR   = 0x%08lX", DCMI->SR);
  LOG_INFO("DCMI RISR = 0x%08lX", DCMI->RISR);
  LOG_INFO("DCMI MISR = 0x%08lX", DCMI->MISR);
  LOG_INFO("DCMI IER  = 0x%08lX", DCMI->IER);

  LOG_INFO("DMA2_Stream1 CR   = 0x%08lX", DMA2_Stream1->CR);
  LOG_INFO("DMA2_Stream1 NDTR = %lu", DMA2_Stream1->NDTR);
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // LOG_DEBUG("System boot, tick = %lu", HAL_GetTick());
    // LOG_INFO("Temperature = %.2f", temp);
    // LOG_WARN("Battery low: %d%%", percentage);
    // LOG_ERROR("Sensor read failed, error = 0x%02X", err);
    //LOG_RAW("This is raw text without any prefix\r\n");
    //PCF8574_WriteBit(PCF8574_IO_P0, 0);   /* 设置 P0 引脚为低电平，测试蜂鸣器 */

    //PCF8574_WriteBit(PCF8574_OV_PWDN_IO, 0);   /* 连接摄像头电源引脚到地，开启摄像头 */

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
