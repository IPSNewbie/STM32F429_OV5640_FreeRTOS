#ifndef ISP_OV5640_CAMERA_CLI_H
#define ISP_OV5640_CAMERA_CLI_H

#include "camera_image_process.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>

typedef enum
{
    CAMERA_CLI_OK = 0,
    CAMERA_CLI_ERROR = 1,
    CAMERA_CLI_ERROR_NULL = 2,
    CAMERA_CLI_ERROR_UNKNOWN_CMD = 3,
    CAMERA_CLI_ERROR_BAD_ARG = 4
} CameraCliStatus_t;

typedef struct
{
    CameraProcessMode_t process_mode;
    uint8_t binary_threshold;
} CameraCliRuntimeConfig_t;

void Camera_CLI_Init(void);
CameraCliStatus_t Camera_CLI_HandleLine(
    UART_HandleTypeDef *huart,
    const char *line);
CameraProcessMode_t Camera_CLI_GetProcessMode(void);
uint8_t Camera_CLI_GetBinaryThreshold(void);
const CameraCliRuntimeConfig_t *Camera_CLI_GetConfig(void);
void Camera_CLI_ResetDefault(void);

#endif /* ISP_OV5640_CAMERA_CLI_H */
