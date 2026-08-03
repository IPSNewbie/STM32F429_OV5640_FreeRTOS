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
#include "cmsis_os.h"
#include "dma.h"
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
#include "camera_cli.h"
#include "camera_frame_buffer.h"
#include "camera_image_process.h"
#include "camera_pc_dump.h"
#include "camera_rtos.h"
#include "OV5640.h"
#include "ov5640_tuning.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CAMERA_MODE_480X320_REAL       0
#define CAMERA_MODE_480X320_TESTBAR    1
#define CAMERA_MODE_320X240_REAL       2
#define CAMERA_MODE_PC_DUMP_RGB565     3

#define CAMERA_MODE                    CAMERA_MODE_PC_DUMP_RGB565
#define PC_DUMP_USE_REAL_IMAGE         1U
#define OV5640_AEC_TUNING_ENABLE       1U
#define OV5640_AEC_TUNING_LEVEL        OV5640_AEC_TARGET_BASELINE
#define OV5640_AWB_TUNING_ENABLE       1U
#define OV5640_AWB_TUNING_MODE         OV5640_AWB_MODE_AUTO
#define OV5640_IMAGE_TUNING_ENABLE     1U
#define OV5640_BRIGHTNESS_LEVEL        1
#define OV5640_CONTRAST_LEVEL          0
#define OV5640_SATURATION_LEVEL        1
#define OV5640_SHARPNESS_LEVEL         0
#define CAMERA_FRAME_BUFFER_ENABLE     1U
#define CAMERA_IMAGE_PROCESS_ENABLE    1U

#define CAMERA_PROCESS_MODE_BYPASS     0
#define CAMERA_PROCESS_MODE_GRAYSCALE  1
#define CAMERA_PROCESS_MODE_BINARY     2

#define CAMERA_PROCESS_MODE            CAMERA_PROCESS_MODE_BYPASS
#define CAMERA_BINARY_THRESHOLD        128U
#define CAMERA_CLI_ENABLE              1U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
static void Camera_Application_Init(void);
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
  MX_DMA_Init();
  MX_I2C2_Init();
  MX_USART1_UART_Init();
  MX_TIM7_Init();
  /* USER CODE BEGIN 2 */
  log_init(&huart1);
  log_set_level(LOG_LEVEL_DEBUG);   // 输出 DEBUG 及以上等级
  Delay_TIM7_Init();
  Camera_Application_Init();
  Camera_RTOS_Init(&huart1);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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

static void Camera_Application_Init(void)
{
  uint16_t ov_id;
  uint8_t ret;

  if (PCF8574_Init())
  {
    LOG_ERROR("PCF8574 initialization failed, check I2C connection");
    HAL_Delay(100);
  }
  else
  {
    LOG_INFO("PCF8574 initialized successfully");
  }

  /* Leave camera power-down mode before applying the hardware reset. */
  PCF8574_WriteBit(PCF8574_OV_PWDN_IO, 0);
  HAL_Delay(20);

  HAL_GPIO_WritePin(OV_RESET_GPIO_Port, OV_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(OV_RESET_GPIO_Port, OV_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(50);

  ov_id = OV5640_ReadID();
  if (ov_id == 0x5640)
  {
    LOG_INFO("OV5640 OK, ID = 0x%04X", ov_id);
  }
  else
  {
    LOG_ERROR("OV5640 ERROR, ID = 0x%04X", ov_id);
  }

#if CAMERA_MODE != CAMERA_MODE_PC_DUMP_RGB565
  LCD_MCU_Init();
  HAL_Delay(1000);
#endif

#if CAMERA_MODE == CAMERA_MODE_PC_DUMP_RGB565
#if PC_DUMP_USE_REAL_IMAGE
  ret = OV5640_Min_InitRGB565_160x120_RealImage();
#else
  ret = OV5640_Min_InitRGB565_160x120_TestBar();
#endif
  if (ret != 0U)
  {
    LOG_ERROR("OV5640 PC dump 160x120 init failed, ret = %u", ret);
  }
#if OV5640_AEC_TUNING_ENABLE
  if (ret == 0U)
  {
    uint8_t aec_target_ret = OV5640_Tuning_SetAecTarget(OV5640_AEC_TUNING_LEVEL);
    if (aec_target_ret != 0U)
    {
      LOG_ERROR("OV5640 AEC target set failed, ret = %u", aec_target_ret);
    }
    HAL_Delay(1000U);
  }
#endif
#if OV5640_AWB_TUNING_ENABLE
  if (ret == 0U)
  {
    uint8_t awb_mode_ret = OV5640_Tuning_SetAWBMode(OV5640_AWB_TUNING_MODE);
    if (awb_mode_ret != 0U)
    {
      LOG_ERROR("OV5640 AWB mode set failed, ret = %u", awb_mode_ret);
    }
  }
#endif
#if OV5640_IMAGE_TUNING_ENABLE
  if (ret == 0U)
  {
    uint8_t brightness_ret = OV5640_Tuning_SetBrightness(OV5640_BRIGHTNESS_LEVEL);
    uint8_t contrast_ret = OV5640_Tuning_SetContrast(OV5640_CONTRAST_LEVEL);
    uint8_t saturation_ret = OV5640_Tuning_SetSaturation(OV5640_SATURATION_LEVEL);
    uint8_t sharpness_ret = OV5640_Tuning_SetSharpness(OV5640_SHARPNESS_LEVEL);
    if ((brightness_ret != 0U) || (contrast_ret != 0U) ||
        (saturation_ret != 0U) || (sharpness_ret != 0U))
    {
      LOG_ERROR("OV5640 image tuning failed B=%u C=%u S=%u H=%u",
                brightness_ret,
                contrast_ret,
                saturation_ret,
                sharpness_ret);
    }
  }
#endif
#elif CAMERA_MODE == CAMERA_MODE_480X320_REAL
  ret = OV5640_Min_InitRGB565_480x320_RealImage();
  if (ret != 0U)
  {
    LOG_ERROR("OV5640 480x320 real image init failed, ret = %u", ret);
  }
#elif CAMERA_MODE == CAMERA_MODE_480X320_TESTBAR
  ret = OV5640_Min_InitRGB565_480x320_TestBar();
  if (ret != 0U)
  {
    LOG_ERROR("OV5640 480x320 testbar init failed, ret = %u", ret);
  }
#elif CAMERA_MODE == CAMERA_MODE_320X240_REAL
  ret = OV5640_Min_InitRGB565_QVGA_RealImage();
  if (ret != 0U)
  {
    LOG_ERROR("OV5640 320x240 real image init failed, ret = %u", ret);
  }
#else
#error "Unsupported CAMERA_MODE"
#endif

#if (CAMERA_FRAME_BUFFER_ENABLE != 0U)
  Camera_FrameBuffer_Init();
#endif
#if (CAMERA_CLI_ENABLE != 0U)
  Camera_CLI_Init();
#endif

  Camera_DCMI_Init();

#if CAMERA_MODE != CAMERA_MODE_PC_DUMP_RGB565
  Camera_DCMI_DMA_ConfigToLCD((uint32_t)LCD_MCU_GetRAMAddress());

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
#endif

  if (ret == 0U)
  {
    LOG_INFO("Camera init OK");
  }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
