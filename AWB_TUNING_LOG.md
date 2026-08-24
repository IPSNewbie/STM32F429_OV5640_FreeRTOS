# OV5640 AWB / Color Tuning Log

## Stage

Stage 2 - AWB / Color Tuning

## Current project state

```text
1. STM32F429 + OV5640 + NT35310 LCD display path is working.
2. 480x320 LCD snow/noise issue has been solved by optimizing FMC/LCD write timing.
3. PC Dump is working. Python can send DUMP\n and receive a 160x120 RGB565 image.
4. AEC / exposure tuning stage has been completed.
5. Current AEC target remains baseline.
```

## Current AEC baseline

```text
0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26
```

## Goal

The goal of this stage is to compare several AWB modes and select a stable color configuration.

This stage focuses only on AWB / color balance.

This stage does not modify:

```text
1. Exposure time
2. Analog gain
3. AEC target
4. Brightness
5. Contrast
6. Saturation
7. Sharpness
8. DCMI/DMA/LCD/PC Dump data path
```

## AWB modes to test

```text
OV5640_AWB_MODE_AUTO
OV5640_AWB_MODE_SUNNY
OV5640_AWB_MODE_CLOUDY
OV5640_AWB_MODE_OFFICE
OV5640_AWB_MODE_HOME
```

## Test method

Each AWB mode is selected at compile time:

```c
#define OV5640_AWB_TUNING_ENABLE      1U
#define OV5640_AWB_TUNING_MODE        OV5640_AWB_MODE_AUTO
```

For each test:

```text
1. Modify OV5640_AWB_TUNING_MODE.
2. Build the firmware.
3. Download the firmware.
4. Reset the board.
5. Run the Python PC Dump tool directly.
6. Record the result in this log.
```

Python commands:

```bash
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_auto
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_sunny
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_cloudy
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_office
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_home
```

## Evaluation metrics

Main metrics:

```text
R/G ratio
B/G ratio
B/R ratio
R mean
G mean
B mean
```

Secondary metrics:

```text
mean_brightness
shadow_ratio
highlight_ratio
Laplacian variance
```

Target range:

```text
B/R ratio: 0.9 ~ 1.15
R/G ratio: close to 1.0
B/G ratio: close to 1.0
White/gray objects should not look obviously blue, red, or yellow.
```

## Known limitation

Round 1 AEC dump recorded a known UART command limitation:

```text
After sending AEC\r\n from serial assistant, the next Python DUMP command may fail to receive OV56RGB5.
```

Therefore, this AWB stage does not use continuous UART commands such as:

```text
AWB -> DUMP
```

Instead, all tests use:

```text
compile-time AWB mode selection -> reset board -> direct Python DUMP
```

## Result table

| AWB mode | Tag             | Mean brightness | Shadow ratio | Highlight ratio | R mean | G mean | B mean | R/G ratio | B/G ratio | B/R ratio | Laplacian variance | Visual observation |
| -------- | --------------- | --------------: | -----------: | --------------: | -----: | -----: | -----: | --------: | --------: | --------: | -----------------: | ------------------ |
| AUTO     | real_awb_auto   |                 |              |                 |        |        |        |           |           |           |                    |                    |
| SUNNY    | real_awb_sunny  |                 |              |                 |        |        |        |           |           |           |                    |                    |
| CLOUDY   | real_awb_cloudy |                 |              |                 |        |        |        |           |           |           |                    |                    |
| OFFICE   | real_awb_office |                 |              |                 |        |        |        |           |           |           |                    |                    |
| HOME     | real_awb_home   |                 |              |                 |        |        |        |           |           |           |                    |                    |

## Round conclusion

Pending.

## Round 1 - AUTO vs OFFICE test result

### Test purpose

This round compares `OV5640_AWB_MODE_AUTO` and `OV5640_AWB_MODE_OFFICE` under the same PC Dump workflow.

The goal is to check whether a fixed manual AWB mode can improve color balance compared with the default automatic AWB mode.

### Test method

Each AWB mode was selected at compile time:

```
#define OV5640_AWB_TUNING_ENABLE      1U
#define OV5640_AWB_TUNING_MODE        OV5640_AWB_MODE_AUTO
```

or:

```
#define OV5640_AWB_TUNING_ENABLE      1U
#define OV5640_AWB_TUNING_MODE        OV5640_AWB_MODE_OFFICE
```

For each test:

```
1. Modify OV5640_AWB_TUNING_MODE in Core/Src/main.c.
2. Build the firmware.
3. Download the firmware.
4. Reset the board.
5. Run the Python PC Dump tool directly.
6. Record the generated summary.csv result.
```

Python commands:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_auto
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_awb_office
```

### Test results

| Index | Time                | AWB mode | Tag             | Image                                            | Mean brightness | Shadow ratio | Highlight ratio | R mean     | G mean    | B mean    | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | -------- | --------------- | ------------------------------------------------ | --------------- | ------------ | --------------- | ---------- | --------- | --------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-01T22:54:14 | AUTO     | real_awb_auto   | captures/007_real_awb_auto_20260701_225414.png   | 91.346771       | 4.104167%    | 6.770833%       | 90.442240  | 90.785885 | 96.412292 | 0.996215  | 1.061974  | 1.066010  | 844.800011         |
| 2     | 2026-07-01T22:54:54 | OFFICE   | real_awb_office | captures/008_real_awb_office_20260701_225454.png | 90.593542       | 6.786458%    | 7.145833%       | 101.174115 | 84.819219 | 92.536198 | 1.192821  | 1.090981  | 0.914623  | 882.121250         |

### Analysis

Compared with `AUTO`, `OFFICE` did not improve the overall color balance.

For `AUTO`:

```
R/G ratio = 0.996215
B/G ratio = 1.061974
B/R ratio = 1.066010
```

`AUTO` keeps the red and green channels very close. The blue channel is slightly higher, but the B/R ratio is still within the acceptable target range of `0.9 ~ 1.15`.

For `OFFICE`:

```
R/G ratio = 1.192821
B/G ratio = 1.090981
B/R ratio = 0.914623
```

`OFFICE` increases the red channel significantly. The R/G ratio is much higher than 1.0, which means the image may become red-biased or yellow-biased.

The brightness metrics also do not favor `OFFICE`:

```
AUTO mean_brightness   = 91.346771
OFFICE mean_brightness = 90.593542

AUTO shadow_ratio      = 4.104167%
OFFICE shadow_ratio    = 6.786458%

AUTO highlight_ratio   = 6.770833%
OFFICE highlight_ratio = 7.145833%
```

`OFFICE` slightly lowers the mean brightness, increases the shadow ratio, and also increases the highlight ratio. Therefore, it does not provide a better overall image quality result.

The Laplacian variance of `OFFICE` is higher:

```
AUTO Laplacian variance   = 844.800011
OFFICE Laplacian variance = 882.121250
```

However, sharpness is not the main optimization target in the AWB stage. The color balance degradation is more important than this small sharpness increase.

### Conclusion

```
1. OV5640_AWB_MODE_AUTO gives the best overall color balance in the current test.
2. AUTO keeps R/G ratio close to 1.0.
3. AUTO keeps B/R ratio within the acceptable range of 0.9 ~ 1.15.
4. OFFICE introduces a clear red/yellow bias because R/G ratio increases to 1.192821.
5. OFFICE also increases both shadow_ratio and highlight_ratio.
6. OFFICE should not be selected as the default AWB mode.
7. SUNNY / CLOUDY / HOME are not tested in this round because AUTO is already acceptable and OFFICE did not show improvement.
```

### Final decision for Stage 2 Round 1

Keep the default AWB mode as:

```
#define OV5640_AWB_TUNING_MODE        OV5640_AWB_MODE_AUTO
```

Current recommended AWB setting:

```
OV5640_AWB_MODE_AUTO
```

The AWB tuning interface will be kept for later experiments, but the current project default should remain automatic AWB.

### Next step

Stage 2 can be closed after restoring `OV5640_AWB_TUNING_MODE` to `OV5640_AWB_MODE_AUTO`.

The next planned stage is:

```
Stage 3 - Brightness / Contrast / Saturation / Sharpness
```

Stage 3 should still follow the same rule:

```
1. Modify only one image parameter at a time.
2. Use compile-time selection first.
3. Use PC Dump summary.csv for comparison.
4. Do not enter UART CLI stage yet.
5. Do not modify DCMI/DMA/LCD/PC Dump protocol.
```