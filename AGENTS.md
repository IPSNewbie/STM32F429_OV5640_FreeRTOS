\# AGENTS.md



\## Project



This is an STM32F429 + OV5640 + FreeRTOS camera project.



Hardware:

\- Board: ALIENTEK Apollo V2 STM32F429IGT6.

\- Camera: ALIENTEK OV5640 module with onboard 24 MHz crystal.

\- LCD: ALIENTEK 3.5-inch MCU resistive touch TFT LCD, LCD ID 0x5310, NT35310.

\- OV5640 SCCB uses software I2C:

&#x20; - SIOC = PB4

&#x20; - SIOD = PB3

\- PCF8574 uses hardware I2C.

&#x20; - OV\_PWDN is controlled by PCF8574\_P2.

\- OV\_RESET = PA15.

\- DCMI/DVP:

&#x20; - D0=PC6, D1=PC7, D2=PC8, D3=PC9

&#x20; - D4=PC11, D5=PD3, D6=PB8, D7=PB9

&#x20; - VSYNC=PB7, HREF=PH8, PCLK=PA6

\- XCLK is not used because the OV5640 module has an onboard 24 MHz crystal.

\- SDIO conflicts with DCMI pins PC8/PC9/PC11. Do not implement SD card real-time saving now.



\## Current working state



The camera display pipeline is already working.



Already completed:

\- PCF8574 init works.

\- OV5640 PWDN/RESET works.

\- SCCB read/write works.

\- OV5640 ID = 0x5640.

\- LCD ID = 0x5310, NT35310.

\- LCD local color bars work.

\- OV5640 full init table + RGB565 table + QVGA config work.

\- DCMI receives frame interrupts.

\- Real OV5640 image is displayed on the MCU LCD.



\## Important files



\- bsp\_sccb.h/.c:

&#x20; - SCCB\_WriteReg(uint16\_t reg, uint8\_t data)

&#x20; - SCCB\_ReadReg(uint16\_t reg, uint8\_t \*data)

&#x20; - OV5640\_ReadID(void)



\- ov5640\_min.c/.h:

&#x20; - OV5640\_Min\_InitRGB565\_QVGA\_TestBar()

&#x20; - OV5640\_Min\_InitRGB565\_QVGA\_RealImage()



\- ov5640cfg.h:

&#x20; - ov5640\_init\_reg\_tbl

&#x20; - ov5640\_rgb565\_reg\_tbl



\- camera\_dcmi\_dma.c/.h:

&#x20; - DCMI + DMA configuration



\- LCD\_MCU driver:

&#x20; - LCD\_MCU\_Init()

&#x20; - LCD\_MCU\_TestSequence()



\## Current goal



Add OV5640 image tuning support.



Create:

\- ov5640\_tuning.c

\- ov5640\_tuning.h



Implement step by step:

\- test pattern

\- register read/write

\- register dump

\- mirror/flip

\- exposure observation

\- exposure compensation

\- AWB mode

\- brightness

\- contrast

\- saturation

\- sharpness



\## Do not modify unless explicitly requested



Do not:

\- rewrite SCCB driver

\- rewrite LCD driver

\- rewrite DCMI/DMA path

\- change pin definitions

\- change working OV5640 init tables

\- add SD card saving

\- add full-screen scaling

\- make large refactors



Prefer:

\- small changes

\- one feature per commit

\- readable C code

\- clear comments for OV5640 registers

\- wrapper functions instead of scattered direct register writes



\## Done criteria



A task is done only when:

\- code builds successfully

\- existing real image display is not broken

\- modified files are listed

\- register changes are explained

\- diff is small and reviewable

