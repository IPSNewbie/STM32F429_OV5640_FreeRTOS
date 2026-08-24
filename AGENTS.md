# AGENTS.md

## Project

This is an STM32F429 + OV5640 + MCU LCD camera project.

The project is used to develop and debug an embedded camera acquisition pipeline based on:

```
STM32F429 -> DCMI -> DMA -> LCD / PC Dump
```

Current project path:

```
D:\MCU+FreeRTOS\STM32_HAL\ISP_Project\ISP_OV5640
```

Current active branch:

```
feature/pc-frame-dump
```

------

## Hardware

Hardware platform:

```
ALIENTEK Apollo V2 STM32F429IGT6 development board
ALIENTEK OV5640 camera module
ALIENTEK 3.5-inch MCU resistive touch TFT LCD
LCD controller: NT35310
LCD ID: 0x5310
```

OV5640 SCCB uses software I2C:

```
SIOC = PB4
SIOD = PB3
```

PCF8574 uses hardware I2C:

```
OV_PWDN = PCF8574_P2
```

OV5640 reset:

```
OV_RESET = PA15
```

OV5640 DVP/DCMI pins:

```
D0 = PC6
D1 = PC7
D2 = PC8
D3 = PC9
D4 = PC11
D5 = PD3
D6 = PB8
D7 = PB9
VSYNC = PB7
HREF = PH8
PCLK = PA6
```

The OV5640 module has an onboard 24 MHz crystal.

```
STM32 XCLK is not used.
```

SDIO conflicts with DCMI pins PC8 / PC9 / PC11.

```
Do not implement SD card real-time saving at the current stage.
```

------

## Current Working State

The camera display and PC dump pipelines are working.

Completed features:

```
1. PCF8574 initialization works.
2. OV5640 PWDN / RESET sequence works.
3. SCCB read/write works.
4. OV5640 ID = 0x5640.
5. LCD ID = 0x5310, NT35310.
6. LCD local color bar test works.
7. OV5640 320x240 RGB565 real image display works.
8. OV5640 480x320 RGB565 full LCD display works.
9. DCMI + DMA directly writing LCD GRAM works.
10. PC Dump snapshot function works.
11. Python/OpenCV image recovery and quality analysis tool works.
```

Current default display mode should remain:

```
#define CAMERA_MODE CAMERA_MODE_480X320_REAL
```

Current PC dump mode:

```
#define CAMERA_MODE CAMERA_MODE_PC_DUMP_RGB565
```

PC dump resolution:

```
160x120 RGB565
```

PC dump command flow:

```
Python opens USART1
Python sends DUMP\n
STM32 captures one RGB565 snapshot
STM32 sends OV56RGB5 + header + payload + CRC
Python receives, verifies CRC, saves image and quality report
```

------

## Important Debug Conclusions

### 480x320 LCD snow/noise issue

The 480x320 display issue was caused by slow FMC/LCD write timing.

Test result:

```
15/15 snow/noise
10/10 snow/noise
8/8 normal
6/6 normal
4/4 normal
3/3 normal
```

Final selected timing:

```
#define LCD_MCU_FAST_WRITE_TIMING_ENABLE  1
#define LCD_MCU_WRITE_ADDRESS_SETUP       6
#define LCD_MCU_WRITE_DATA_SETUP          6
```

Do not revert this timing unless explicitly testing LCD write timing again.

------

## Current Main Goal

The current development stage is:

```
OV5640 image quality tuning
```

Current priority:

```
Exposure / AEC tuning
```

Current baseline result:

```
Resolution: 160x120
Payload length: 38400 bytes
Mean brightness: 106.251
Min brightness: 8
Max brightness: 255
Shadow ratio: 0.229%
Highlight ratio: 8.604%
R mean: 105.354
G mean: 104.549
B mean: 117.279
R/G ratio: 1.008
B/G ratio: 1.122
B/R ratio: 1.113
R/B ratio: 0.898
Laplacian variance: 623.007
Blur threshold: 100.0
```

Current tuning target:

```
Highlight ratio: reduce from 8.604% to about 3%~5%
Mean brightness: keep around 90~130
B/R ratio: keep around 0.9~1.15
Laplacian variance: should not obviously decrease
```

------

## Important Files

SCCB driver:

```
bsp_sccb.h
bsp_sccb.c
```

Key SCCB APIs:

```
SCCB_WriteReg(uint16_t reg, uint8_t data)
SCCB_ReadReg(uint16_t reg, uint8_t *data)
OV5640_ReadID(void)
```

OV5640 driver:

```
BSPDrivers/Inc/OV5640.h
BSPDrivers/Src/OV5640.c
BSPDrivers/Inc/ov5640cfg.h
```

Camera DCMI / DMA:

```
BSPDrivers/Inc/camera_dcmi_dma.h
BSPDrivers/Src/camera_dcmi_dma.c
```

PC Dump:

```
BSPDrivers/Inc/camera_pc_dump.h
BSPDrivers/Src/camera_pc_dump.c
tools/pc_dump_rgb565.py
PC_DUMP_LOG.md
```

LCD MCU driver:

```
LCD_MCU driver for NT35310
```

Current tuning files to be added later:

```
BSPDrivers/Inc/ov5640_tuning.h
BSPDrivers/Src/ov5640_tuning.c
AEC_TUNING_LOG.md
```

------

## Development Rules

Do not modify unless explicitly requested:

```
1. Do not rewrite the SCCB driver.
2. Do not rewrite the LCD driver.
3. Do not rewrite the DCMI/DMA display path.
4. Do not change GPIO pin definitions.
5. Do not change the working OV5640 init tables unless necessary.
6. Do not add SD card saving at the current stage.
7. Do not add framebuffer-based scaling.
8. Do not make large refactors.
```

Prefer:

```
1. Small changes.
2. One feature per commit.
3. One tuning parameter per test.
4. Clear register comments.
5. Wrapper functions instead of scattered direct register writes.
6. PC Dump based quantitative comparison.
7. Logs for every tuning experiment.
```

------

## Image Tuning Rules

During the AEC/exposure tuning stage:

```
1. Do not modify AWB, saturation, contrast, brightness, or sharpness at the same time.
2. Do not manually write exposure registers before AEC target tests are complete.
3. Do not change DCMI, DMA, LCD, or FMC timing.
4. Do not change image size during one tuning round.
5. Keep the same scene, lighting, camera position, and tag format during comparison.
```

AEC tuning should start from:

```
1. Dump current AEC/AGC registers.
2. Adjust AEC target slightly.
3. Capture using PC Dump.
4. Compare summary.csv metrics.
5. Write result to AEC_TUNING_LOG.md.
```

Main metrics:

```
mean_brightness
highlight_ratio
shadow_ratio
B/R ratio
Laplacian variance
```

------

## Done Criteria

A task is done only when:

```
1. Code builds successfully.
2. Existing 480x320 real image display is not broken.
3. PC Dump still works if related files were touched.
4. Modified files are listed.
5. Register changes are explained.
6. Test result is written to log.
7. Diff is small and reviewable.
```