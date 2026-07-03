# OV5640 Image Parameter Tuning Log

## Stage

Stage 3 - Brightness / Contrast / Saturation / Sharpness

## Current project state

```text
1. STM32F429 + OV5640 + NT35310 LCD display path is working.
2. 480x320 LCD snow/noise issue has been solved by optimizing FMC/LCD write timing.
3. PC Dump is working. Python can send DUMP\n and receive a 160x120 RGB565 image.
4. AEC / exposure tuning stage has been completed.
5. AWB / color tuning stage has been completed.
6. Current stage focuses only on basic image parameters.
```

## Current baseline configuration

AEC target:

```text
OV5640_AEC_TARGET_BASELINE

0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26
```

AWB mode:

```text
OV5640_AWB_MODE_AUTO
```

## Goal

The goal of this stage is to add basic image parameter tuning interfaces for:

```text
1. Brightness
2. Contrast
3. Saturation
4. Sharpness
```

This stage uses PC Dump metrics to compare different settings and select stable defaults.

This stage does not modify:

```text
1. Exposure time
2. Analog gain
3. AEC target
4. AWB mode
5. DCMI/DMA/LCD/PC Dump data path
6. UART CLI
7. FreeRTOS
8. SD card save
```

## Compile-time parameters

```c
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```

## Initial preset note

No exact project-specific brightness / contrast / saturation / sharpness table was found in the current OV5640 driver.

The current SDE/CIP register values are initial presets and must be validated with PC Dump before selecting final defaults.

## Test method

Each image parameter is selected at compile time.

For each test:

```text
1. Modify only one parameter.
2. Keep the other three parameters at default.
3. Build the firmware.
4. Download the firmware.
5. Reset the board.
6. Run the Python PC Dump tool directly.
7. Record the result in this log.
```

Python commands:

```bash
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_img_default
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_bright_plus1
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_contrast_plus1
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_sat_plus1
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_sharp_1
```

## Parameter list

| Parameter  | Supported levels | Default |
| ---------- | ---------------- | ------: |
| Brightness | -1 / 0 / +1      |       0 |
| Contrast   | -1 / 0 / +1      |       0 |
| Saturation | 0 / 1 / 2        |       1 |
| Sharpness  | 0 / 1 / 2        |       0 |

## Evaluation metrics

Main metrics:

```text
mean_brightness
shadow_ratio
highlight_ratio
R/G ratio
B/G ratio
B/R ratio
Laplacian variance
```

Expected behavior:

```text
Brightness:
- mean_brightness should change clearly.
- highlight_ratio should not increase too much.
- shadow_ratio should not become worse.

Contrast:
- image contrast may improve visually.
- highlight_ratio and shadow_ratio may both increase.
- avoid excessive clipping.

Saturation:
- color may become stronger.
- B/R ratio, R/G ratio, and B/G ratio should not become unreasonable.

Sharpness:
- Laplacian variance may increase.
- avoid strong noise, false edges, or harsh image texture.
```

## Known limitation

Round 1 AEC dump recorded a known UART command limitation:

```text
After sending AEC\r\n from serial assistant, the next Python DUMP command may fail to receive OV56RGB5.
```

Therefore, this stage does not use continuous UART commands such as:

```text
bright -> dump
contrast -> dump
sat -> dump
sharp -> dump
```

Instead, all tests use:

```text
compile-time parameter selection -> reset board -> direct Python DUMP
```

## Result table

| Test item     | Tag                 | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R/G ratio | B/G ratio | B/R ratio | Laplacian variance | Visual observation |
| ------------- | ------------------- | ---------: | -------: | ---------: | --------: | --------------: | -----------: | --------------: | --------: | --------: | --------: | -----------------: | ------------------ |
| Default       | real_img_default    |          0 |        0 |          1 |         0 |                 |              |                 |           |           |           |                    |                    |
| Brightness +1 | real_bright_plus1   |         +1 |        0 |          1 |         0 |                 |              |                 |           |           |           |                    |                    |
| Contrast +1   | real_contrast_plus1 |          0 |       +1 |          1 |         0 |                 |              |                 |           |           |           |                    |                    |
| Saturation +1 | real_sat_plus1      |          0 |        0 |          2 |         0 |                 |              |                 |           |           |           |                    |                    |
| Sharpness 1   | real_sharp_1        |          0 |        0 |          1 |         1 |                 |              |                 |           |           |           |                    |                    |

## Round conclusion

Pending.

## Round 1 - Default vs Brightness +1 test result

### Test purpose

This round compares the default image parameter setting and `Brightness +1`.

The goal is to check whether increasing brightness can improve the low mean brightness and high shadow ratio without causing too much highlight clipping.

### Test method

The test uses compile-time image parameter selection.

Default setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```

Brightness +1 setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          1
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```

Python commands:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_img_default
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_bright_plus1
```

### Test results

| Index | Time                | Test item     | Tag               | Image                                              | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean     | B mean     | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ------------- | ----------------- | -------------------------------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | ---------- | ---------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-03T22:36:41 | Default       | real_img_default  | captures/009_real_img_default_20260703_223641.png  | 0          | 0        | 1          | 0         | 84.106302       | 14.010417%   | 6.635417%       | 81.677813 | 84.745885  | 86.826198  | 0.963797  | 1.024548  | 1.063033  | 883.653465         |
| 2     | 2026-07-03T22:37:54 | Brightness +1 | real_bright_plus1 | captures/010_real_bright_plus1_20260703_223754.png | +1         | 0        | 1          | 0         | 99.207865       | 0.000000%    | 8.515625%       | 96.413542 | 100.014740 | 102.093281 | 0.963993  | 1.020782  | 1.058910  | 841.923733         |

### Analysis

Compared with the default setting, `Brightness +1` clearly increases the overall image brightness:

```
mean_brightness:
Default       = 84.106302
Brightness +1 = 99.207865
```

The default mean brightness is below the target range of `90 ~ 130`, while `Brightness +1` brings the image into the expected range.

`Brightness +1` also greatly reduces the shadow ratio:

```
shadow_ratio:
Default       = 14.010417%
Brightness +1 = 0.000000%
```

This means the image is no longer dominated by dark pixels after brightness is increased.

However, `Brightness +1` also increases the highlight ratio:

```
highlight_ratio:
Default       = 6.635417%
Brightness +1 = 8.515625%
```

The highlight ratio becomes worse and moves farther away from the preferred range of `3% ~ 5%`. This means `Brightness +1` improves dark areas but increases highlight clipping risk.

Color balance remains acceptable:

```
B/R ratio:
Default       = 1.063033
Brightness +1 = 1.058910
```

Both values are within the target range of `0.9 ~ 1.15`, and the change is very small. Therefore, `Brightness +1` does not obviously damage color balance.

The Laplacian variance decreases slightly:

```
Laplacian variance:
Default       = 883.653465
Brightness +1 = 841.923733
```

The decrease is not severe, but `Brightness +1` does not improve sharpness.

### Conclusion

```
1. The default setting is too dark under the current test scene.
2. Brightness +1 successfully increases mean_brightness from 84.106302 to 99.207865.
3. Brightness +1 reduces shadow_ratio from 14.010417% to 0.000000%.
4. Brightness +1 does not obviously damage color balance.
5. Brightness +1 increases highlight_ratio from 6.635417% to 8.515625%, so it increases overexposure risk.
6. Brightness +1 is useful, but it should not be selected as the final default before comparing contrast, saturation, and sharpness results.
```

### Temporary decision

Do not finalize the default image parameter setting yet.

Current candidate:

```
Brightness +1 can improve the dark-image problem, but it increases highlight clipping.
```

Next test:

```
Contrast +1
```

For the next test, restore brightness to `0` and only modify contrast:

```
#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            1
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```


## Round 2 - Contrast +1 test result

### Test purpose

This round tests `Contrast +1` while keeping the other image parameters at their default values.

The goal is to check whether increasing contrast can improve image detail and sharpness without causing excessive shadow clipping or highlight clipping.

### Test method

The test uses compile-time image parameter selection.

Contrast +1 setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            1
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```

Python command:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_contrast_plus1
```

### Test result

| Index | Time                | Test item   | Tag                 | Image                                                | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean    | B mean    | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ----------- | ------------------- | ---------------------------------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | --------- | --------- | --------- | --------- | --------- | ------------------ |
| 2     | 2026-07-03T22:43:24 | Contrast +1 | real_contrast_plus1 | captures/014_real_contrast_plus1_20260703_224324.png | 0          | +1       | 1          | 0         | 92.178646       | 25.166667%   | 13.151042%      | 89.535625 | 93.503646 | 91.969323 | 0.957563  | 0.983591  | 1.027181  | 1123.033387        |

### Comparison with previous results

| Test item     | Mean brightness | Shadow ratio | Highlight ratio | B/R ratio | Laplacian variance |
| ------------- | --------------- | ------------ | --------------- | --------- | ------------------ |
| Default       | 84.106302       | 14.010417%   | 6.635417%       | 1.063033  | 883.653465         |
| Brightness +1 | 99.207865       | 0.000000%    | 8.515625%       | 1.058910  | 841.923733         |
| Contrast +1   | 92.178646       | 25.166667%   | 13.151042%      | 1.027181  | 1123.033387        |

### Analysis

`Contrast +1` increases the mean brightness into the target range:

```
mean_brightness:
Default     = 84.106302
Contrast +1 = 92.178646
```

This seems positive at first, because the default image is slightly dark.

`Contrast +1` also significantly increases the Laplacian variance:

```
Laplacian variance:
Default     = 883.653465
Contrast +1 = 1123.033387
```

This indicates that the image has stronger local intensity changes after contrast enhancement. The image may look sharper or stronger visually.

However, the clipping indicators become much worse:

```
shadow_ratio:
Default     = 14.010417%
Contrast +1 = 25.166667%

highlight_ratio:
Default     = 6.635417%
Contrast +1 = 13.151042%
```

Both dark pixels and over-bright pixels increase significantly. This means `Contrast +1` expands the image tone too aggressively under the current scene. It may make the image look more dramatic, but it also increases the risk of losing both shadow detail and highlight detail.

Color balance remains acceptable:

```
R/G ratio = 0.957563
B/G ratio = 0.983591
B/R ratio = 1.027181
```

The B/R ratio is within the acceptable range of `0.9 ~ 1.15`, so `Contrast +1` does not cause obvious blue/red color imbalance. However, color balance is not enough to justify the setting because the shadow and highlight clipping become too severe.

### Conclusion

```
1. Contrast +1 increases mean_brightness from 84.106302 to 92.178646.
2. Contrast +1 increases Laplacian variance from 883.653465 to 1123.033387.
3. Contrast +1 also increases shadow_ratio from 14.010417% to 25.166667%.
4. Contrast +1 increases highlight_ratio from 6.635417% to 13.151042%.
5. The clipping behavior becomes significantly worse.
6. Contrast +1 should not be selected as the default image parameter.
```

### Temporary decision

Do not use `Contrast +1` as the default setting.

Current comparison:

```
Brightness +1:
- Improves mean brightness.
- Reduces shadow ratio.
- Increases highlight ratio.

Contrast +1:
- Improves Laplacian variance.
- But greatly increases both shadow ratio and highlight ratio.
- Not suitable as default.
```

Next test:

```
Saturation +1
```

For the next test, restore brightness and contrast to default, and only modify saturation:

```
#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          2
#define OV5640_SHARPNESS_LEVEL           0
```

## Round 3 - Saturation +1 test result

### Test purpose

This round tests `Saturation +1` while keeping brightness, contrast, and sharpness at their default values.

The goal is to check whether increasing saturation can improve color appearance without damaging brightness, highlight ratio, shadow ratio, or color balance.

### Test method

The test uses compile-time image parameter selection.

Saturation +1 setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          2
#define OV5640_SHARPNESS_LEVEL           0
```

Python command:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_sat_plus1
```

### Test result

| Index | Time                | Test item     | Tag            | Image                                           | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean    | B mean    | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ------------- | -------------- | ----------------------------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | --------- | --------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-03T22:51:36 | Saturation +1 | real_sat_plus1 | captures/015_real_sat_plus1_20260703_225136.png | 0          | 0        | 2          | 0         | 84.027031       | 12.114583%   | 6.427083%       | 81.776250 | 84.880990 | 85.148646 | 0.963422  | 1.003153  | 1.041239  | 872.941699         |

### Comparison with previous results

| Test item     | Mean brightness | Shadow ratio | Highlight ratio | B/R ratio | Laplacian variance |
| ------------- | --------------- | ------------ | --------------- | --------- | ------------------ |
| Default       | 84.106302       | 14.010417%   | 6.635417%       | 1.063033  | 883.653465         |
| Brightness +1 | 99.207865       | 0.000000%    | 8.515625%       | 1.058910  | 841.923733         |
| Contrast +1   | 92.178646       | 25.166667%   | 13.151042%      | 1.027181  | 1123.033387        |
| Saturation +1 | 84.027031       | 12.114583%   | 6.427083%       | 1.041239  | 872.941699         |

### Analysis

`Saturation +1` has very little impact on the overall brightness:

```
mean_brightness:
Default       = 84.106302
Saturation +1 = 84.027031
```

This means saturation adjustment does not significantly affect the exposure or brightness level.

Compared with the default setting, `Saturation +1` slightly reduces the shadow ratio:

```
shadow_ratio:
Default       = 14.010417%
Saturation +1 = 12.114583%
```

The improvement is not large, but it is in a positive direction.

`Saturation +1` also slightly reduces the highlight ratio:

```
highlight_ratio:
Default       = 6.635417%
Saturation +1 = 6.427083%
```

This is also a small improvement. Unlike `Brightness +1`, saturation adjustment does not increase highlight clipping.

Color balance remains acceptable:

```
B/R ratio:
Default       = 1.063033
Saturation +1 = 1.041239
```

`Saturation +1` makes the B/R ratio slightly closer to 1.0. This means it does not introduce obvious blue or red color bias under the current test scene.

The Laplacian variance decreases only slightly:

```
Laplacian variance:
Default       = 883.653465
Saturation +1 = 872.941699
```

The decrease is small and acceptable.

### Conclusion

```
1. Saturation +1 does not significantly change mean_brightness.
2. Saturation +1 slightly reduces shadow_ratio from 14.010417% to 12.114583%.
3. Saturation +1 slightly reduces highlight_ratio from 6.635417% to 6.427083%.
4. Saturation +1 improves B/R ratio from 1.063033 to 1.041239.
5. Saturation +1 does not obviously degrade sharpness.
6. Saturation +1 is safer than Brightness +1 and Contrast +1.
7. Saturation +1 can be kept as a candidate setting, but it cannot solve the low mean_brightness problem.
```

### Temporary decision

`Saturation +1` is a safe candidate.

Current comparison:

```
Brightness +1:
- Improves mean brightness.
- Removes shadow pixels.
- Increases highlight clipping.

Contrast +1:
- Increases Laplacian variance.
- Greatly increases both shadow_ratio and highlight_ratio.
- Not suitable as default.

Saturation +1:
- Slightly improves color balance.
- Slightly reduces shadow_ratio and highlight_ratio.
- Does not solve low mean_brightness.
- Safe as a candidate.
```

Next test:

```
Sharpness 1
```

For the next test, restore brightness and contrast to default, keep saturation at the current default value, and only modify sharpness:

```
#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           1
```

## Round 4 - Sharpness 1 test result

### Test purpose

This round tests `Sharpness 1` while keeping brightness, contrast, and saturation at their default values.

The goal is to check whether increasing sharpness can improve image detail without increasing highlight clipping, shadow clipping, color imbalance, or noise-like artifacts.

### Test method

The test uses compile-time image parameter selection.

Sharpness 1 setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          0
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           1
```

Python command:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_sharp_1
```

### Test result

| Index | Time                | Test item   | Tag          | Image                                         | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean    | B mean    | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ----------- | ------------ | --------------------------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | --------- | --------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-03T22:55:13 | Sharpness 1 | real_sharp_1 | captures/017_real_sharp_1_20260703_225513.png | 0          | 0        | 1          | 1         | 85.850313       | 12.817708%   | 6.953125%       | 82.919531 | 86.779010 | 88.305000 | 0.955525  | 1.017585  | 1.064948  | 874.972616         |

### Comparison with previous results

| Test item     | Mean brightness | Shadow ratio | Highlight ratio | B/R ratio | Laplacian variance |
| ------------- | --------------- | ------------ | --------------- | --------- | ------------------ |
| Default       | 84.106302       | 14.010417%   | 6.635417%       | 1.063033  | 883.653465         |
| Brightness +1 | 99.207865       | 0.000000%    | 8.515625%       | 1.058910  | 841.923733         |
| Contrast +1   | 92.178646       | 25.166667%   | 13.151042%      | 1.027181  | 1123.033387        |
| Saturation +1 | 84.027031       | 12.114583%   | 6.427083%       | 1.041239  | 872.941699         |
| Sharpness 1   | 85.850313       | 12.817708%   | 6.953125%       | 1.064948  | 874.972616         |

### Analysis

`Sharpness 1` slightly increases the mean brightness compared with the default setting:

```
mean_brightness:
Default     = 84.106302
Sharpness 1 = 85.850313
```

However, the mean brightness is still below the target range of `90 ~ 130`, so sharpness adjustment does not solve the low-brightness problem.

The shadow ratio is slightly improved:

```
shadow_ratio:
Default     = 14.010417%
Sharpness 1 = 12.817708%
```

This is a small positive change.

The highlight ratio becomes slightly worse:

```
highlight_ratio:
Default     = 6.635417%
Sharpness 1 = 6.953125%
```

This means `Sharpness 1` slightly increases the overexposed pixel ratio.

Color balance remains acceptable:

```
B/R ratio:
Default     = 1.063033
Sharpness 1 = 1.064948
```

The change is very small, and both values are within the acceptable range of `0.9 ~ 1.15`.

The most important result is the Laplacian variance:

```
Laplacian variance:
Default     = 883.653465
Sharpness 1 = 874.972616
```

`Sharpness 1` does not increase the Laplacian variance. Instead, it slightly decreases it. Therefore, under the current test condition, `Sharpness 1` does not provide measurable sharpness improvement.

### Conclusion

```
1. Sharpness 1 does not solve the low mean_brightness problem.
2. Sharpness 1 slightly reduces shadow_ratio, but the improvement is limited.
3. Sharpness 1 slightly increases highlight_ratio.
4. Sharpness 1 does not damage color balance.
5. Sharpness 1 does not improve Laplacian variance.
6. Sharpness 1 should not be selected as the default setting.
```

### Temporary decision

Do not use `Sharpness 1` as the default setting.

Current comparison:

```
Brightness +1:
- Improves mean brightness.
- Reduces shadow_ratio to 0%.
- Increases highlight_ratio.

Contrast +1:
- Increases Laplacian variance.
- Greatly increases both shadow_ratio and highlight_ratio.
- Not suitable as default.

Saturation +1:
- Slightly improves color balance.
- Slightly reduces shadow_ratio and highlight_ratio.
- Safe candidate.

Sharpness 1:
- Does not improve Laplacian variance.
- Slightly increases highlight_ratio.
- Not suitable as default.
```

### Next test

The next test should evaluate a combined candidate:

```
Brightness +1 + Saturation +1
```

Reason:

```
1. Brightness +1 solves the low mean_brightness problem.
2. Saturation +1 is the safest color-related improvement.
3. Contrast +1 and Sharpness 1 are not recommended.
4. The combined test can check whether Brightness +1 and Saturation +1 can work together without making highlight_ratio unacceptable.
```

For the next test, use:

```
#define OV5640_BRIGHTNESS_LEVEL          1
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          2
#define OV5640_SHARPNESS_LEVEL           0
```

Python command:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_bright_plus1_sat_plus1
```

## Round 5 - Brightness +1 + Saturation +1 combined test result

### Test purpose

This round tests the combined candidate setting:

```
Brightness +1 + Saturation +1
```

The goal is to check whether `Saturation +1` can be combined with `Brightness +1` to improve color appearance while keeping the brightness improvement from `Brightness +1`.

### Test method

The test uses compile-time image parameter selection.

Combined setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          1
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          2
#define OV5640_SHARPNESS_LEVEL           0
```

Python command:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_bright_plus1_sat_plus1
```

### Test result

| Index | Time                | Test item                     | Tag                         | Image                                                        | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean     | B mean     | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ----------------------------- | --------------------------- | ------------------------------------------------------------ | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | ---------- | ---------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-03T22:58:16 | Brightness +1 + Saturation +1 | real_bright_plus1_sat_plus1 | captures/018_real_bright_plus1_sat_plus1_20260703_225816.png | +1         | 0        | 2          | 0         | 100.566302      | 0.000000%    | 8.640625%       | 98.025573 | 100.765417 | 105.955885 | 0.972810  | 1.051510  | 1.080900  | 822.510609         |

### Comparison with previous results

| Test item                     | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | B/R ratio | Laplacian variance |
| ----------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | ------------------ |
| Default                       | 0          | 0        | 1          | 0         | 84.106302       | 14.010417%   | 6.635417%       | 1.063033  | 883.653465         |
| Brightness +1                 | +1         | 0        | 1          | 0         | 99.207865       | 0.000000%    | 8.515625%       | 1.058910  | 841.923733         |
| Contrast +1                   | 0          | +1       | 1          | 0         | 92.178646       | 25.166667%   | 13.151042%      | 1.027181  | 1123.033387        |
| Saturation +1                 | 0          | 0        | 2          | 0         | 84.027031       | 12.114583%   | 6.427083%       | 1.041239  | 872.941699         |
| Sharpness 1                   | 0          | 0        | 1          | 1         | 85.850313       | 12.817708%   | 6.953125%       | 1.064948  | 874.972616         |
| Brightness +1 + Saturation +1 | +1         | 0        | 2          | 0         | 100.566302      | 0.000000%    | 8.640625%       | 1.080900  | 822.510609         |

### Analysis

The combined setting successfully keeps the brightness improvement from `Brightness +1`:

```
mean_brightness:
Default                          = 84.106302
Brightness +1                    = 99.207865
Brightness +1 + Saturation +1     = 100.566302
```

The mean brightness is within the target range of `90 ~ 130`.

The combined setting also keeps the shadow ratio at zero:

```
shadow_ratio:
Brightness +1                    = 0.000000%
Brightness +1 + Saturation +1     = 0.000000%
```

This means the dark-area problem is still solved.

However, the highlight ratio becomes slightly worse than using `Brightness +1` alone:

```
highlight_ratio:
Brightness +1                    = 8.515625%
Brightness +1 + Saturation +1     = 8.640625%
```

The increase is not large, but it shows that the combined setting does not improve highlight clipping.

The color balance also becomes slightly more blue-biased:

```
B/R ratio:
Brightness +1                    = 1.058910
Brightness +1 + Saturation +1     = 1.080900
```

Both values are still within the acceptable range of `0.9 ~ 1.15`, but the combined setting is farther from 1.0 than `Brightness +1` alone.

The Laplacian variance becomes lower:

```
Laplacian variance:
Brightness +1                    = 841.923733
Brightness +1 + Saturation +1     = 822.510609
```

This means the combined setting does not improve the sharpness metric. It is slightly worse than using `Brightness +1` alone.

### Conclusion

```
1. Brightness +1 + Saturation +1 keeps the mean brightness in the target range.
2. It keeps shadow_ratio at 0.000000%.
3. It slightly increases highlight_ratio compared with Brightness +1 alone.
4. It slightly increases B/R ratio, making the image a little more blue-biased.
5. It reduces Laplacian variance compared with Brightness +1 alone.
6. Therefore, the combined setting is not better than Brightness +1 alone.
7. Brightness +1 + Saturation +1 should not be selected as the final default setting.
```

## Final conclusion for Stage 3

### Summary of all tested settings

| Test item                     | Brightness | Contrast | Saturation | Sharpness | Mean brightness | Shadow ratio | Highlight ratio | B/R ratio | Laplacian variance | Decision                      |
| ----------------------------- | ---------- | -------- | ---------- | --------- | --------------- | ------------ | --------------- | --------- | ------------------ | ----------------------------- |
| Default                       | 0          | 0        | 1          | 0         | 84.106302       | 14.010417%   | 6.635417%       | 1.063033  | 883.653465         | Too dark                      |
| Brightness +1                 | +1         | 0        | 1          | 0         | 99.207865       | 0.000000%    | 8.515625%       | 1.058910  | 841.923733         | Recommended                   |
| Contrast +1                   | 0          | +1       | 1          | 0         | 92.178646       | 25.166667%   | 13.151042%      | 1.027181  | 1123.033387        | Not recommended               |
| Saturation +1                 | 0          | 0        | 2          | 0         | 84.027031       | 12.114583%   | 6.427083%       | 1.041239  | 872.941699         | Safe but still dark           |
| Sharpness 1                   | 0          | 0        | 1          | 1         | 85.850313       | 12.817708%   | 6.953125%       | 1.064948  | 874.972616         | Not recommended               |
| Brightness +1 + Saturation +1 | +1         | 0        | 2          | 0         | 100.566302      | 0.000000%    | 8.640625%       | 1.080900  | 822.510609         | Not better than Brightness +1 |

### Final recommended image parameter setting

Use only `Brightness +1` as the current default image tuning setting:

```
#define OV5640_IMAGE_TUNING_ENABLE       1U

#define OV5640_BRIGHTNESS_LEVEL          1
#define OV5640_CONTRAST_LEVEL            0
#define OV5640_SATURATION_LEVEL          1
#define OV5640_SHARPNESS_LEVEL           0
```

### Reason

```
1. The original default image is too dark under the current test scene.
2. Brightness +1 increases mean_brightness from 84.106302 to 99.207865.
3. Brightness +1 reduces shadow_ratio from 14.010417% to 0.000000%.
4. Brightness +1 keeps B/R ratio within the acceptable range.
5. Contrast +1 causes excessive shadow and highlight clipping.
6. Saturation +1 is safe but does not solve the low-brightness problem.
7. Sharpness 1 does not improve Laplacian variance.
8. Brightness +1 + Saturation +1 is not better than Brightness +1 alone.
```

### Known limitation of the selected setting

```
Brightness +1 increases highlight_ratio from 6.635417% to 8.515625%.
```

This means the selected setting improves dark areas and overall brightness, but it also increases highlight clipping risk. The current choice is a practical trade-off for better overall visibility.

### Final decision

Stage 3 can be closed with the following default:

```
Brightness = +1
Contrast   = 0
Saturation = 1
Sharpness  = 0
```

The next planned stage is:

```
Stage 4 - Frame Buffer Management / Double Buffer
```

Stage 4 should focus on frame buffer structure and small-resolution double buffering first. It should not modify AEC, AWB, brightness, contrast, saturation, sharpness, or the PC Dump protocol.