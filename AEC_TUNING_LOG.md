# AEC Tuning Log

## Round 1 - AEC/AGC register dump

### Goal

Only add an AEC/AGC register dump command. Do not change exposure parameters or image tuning parameters.

### Modified files

- `BSPDrivers/Inc/ov5640_tuning.h`
- `BSPDrivers/Src/ov5640_tuning.c`
- `BSPDrivers/Inc/camera_pc_dump.h`
- `BSPDrivers/Src/camera_pc_dump.c`
- `Core/Src/main.c`
- `CMakeLists.txt`
- `AEC_TUNING_LOG.md`

### New command

- `AEC\n`: print OV5640 AEC/AGC register values as text only.

### Test method

1. Use a serial terminal to send `AEC\n`.
2. Confirm the board prints the AEC/AGC text dump and does not send an `OV56RGB5` image packet.
3. Use `tools/pc_dump_rgb565.py` or the existing host flow to send `DUMP\n`.
4. Confirm the PC Dump image packet is still received and decoded normally.

### Acceptance criteria

- `AEC\n` only outputs text register dump.
- `DUMP\n` still outputs the existing `OV56RGB5` image packet.
- SCCB, LCD, DCMI, DMA, FMC timing, GPIO, PCF8574/PWDN/RESET, and image tuning parameters are not modified.

## Round 1 validation result - AEC dump command

### Test environment

```
Branch: feature/ov5640-aec-tuning
Camera mode: CAMERA_MODE_PC_DUMP_RGB565
PC dump image size: 160x120 RGB565
PC command: DUMP\r\n
AEC dump command: AEC\r\n
```

### Round 1 goal

```
Only add OV5640 AEC/AGC register dump command.
Do not modify exposure parameters.
Do not modify AWB, brightness, contrast, saturation or sharpness.
Do not modify SCCB/LCD/DCMI/DMA main path.
Do not modify OV56RGB5 PC dump packet protocol.
```

### Implemented changes

```
1. Added BSPDrivers/Inc/ov5640_tuning.h.
2. Added BSPDrivers/Src/ov5640_tuning.c.
3. Added OV5640_Tuning_DumpAECRegs().
4. Added OV5640_Tuning_GetExposureRaw().
5. Added OV5640_Tuning_GetGainRaw().
6. Added AEC command recognition in PC Dump command parser.
7. Kept original DUMP command and OV56RGB5 image packet protocol unchanged.
```

### AEC dump result

```
========== OV5640 AEC/AGC DUMP ==========
EXPOSURE_H   0x3500 = 0x00
EXPOSURE_M   0x3501 = 0x7B
EXPOSURE_L   0x3502 = 0x00
Exposure raw = 0x07B00
AEC_AGC_CTRL 0x3503 = 0x00
GAIN_H       0x350A = 0x00
GAIN_L       0x350B = 0xF8
Gain raw     = 0x0F8
AEC_CTRL_00  0x3A00 = 0x78
AEC_WPT      0x3A0F = 0x30
AEC_BPT      0x3A10 = 0x28
AEC_WPT2     0x3A1B = 0x30
AEC_BPT2     0x3A1E = 0x26
GAIN_CEIL_H  0x3A18 = 0x00
GAIN_CEIL_L  0x3A19 = 0xF8
=========================================
```

### Register observation

```
1. 0x3503 = 0x00, so AEC and AGC are both in automatic mode.
2. Current exposure raw value is 0x07B00.
3. Current gain raw value is 0x0F8.
4. Gain ceiling is also 0x00F8.
5. Current AEC target related registers are:
   0x3A0F = 0x30
   0x3A10 = 0x28
   0x3A1B = 0x30
   0x3A1E = 0x26
6. Therefore, later AEC target tuning must use 0x30/0x28/0x30/0x26 as the real baseline, not the previously assumed 0x78/0x68 values.
```

### Passed tests

```
1. Python sends DUMP\r\n after board reset: OK.
2. STM32 sends OV56RGB5 image packet after DUMP command: OK.
3. Python receives image payload and CRC successfully when DUMP is used alone: OK.
4. Serial assistant sends AEC\r\n: OK.
5. STM32 prints OV5640 AEC/AGC register dump after AEC command: OK.
6. AEC command does not send OV56RGB5 image packet: OK.
7. ov5640_tuning.c only reads OV5640 registers and does not write exposure/AEC/AWB/color registers: OK.
```

### Known limitation

```
DUMP -> AEC works, but AEC -> DUMP does not work reliably.

Current observed behavior:
1. After board reset, Python DUMP command works.
2. After DUMP, serial assistant sends AEC command successfully.
3. After AEC command, Python DUMP command may fail to receive OV56RGB5 within timeout.
4. Test 2: DUMP -> AEC -> DUMP failed.
5. Test 3: AEC -> DUMP failed.
```

### Temporary workaround

```
For the next AEC tuning round, do not rely on continuous AEC -> DUMP command sequence.

Use the following workflow instead:
1. Modify AEC target setting in firmware.
2. Compile and download firmware.
3. Reset the board.
4. Run Python DUMP directly.
5. Record summary.csv result.
6. Repeat for the next AEC parameter set.

The AEC command is currently kept as a one-shot diagnostic command.
After using AEC command, reset the board before running Python DUMP again.
```

### Decision

```
Round 1 is accepted as partially passed.

Accepted part:
1. AEC/AGC register dump command is implemented.
2. AEC/AGC registers can be read successfully.
3. DUMP command still works when used directly after reset.
4. OV56RGB5 packet protocol is not modified.

Known unresolved issue:
1. Continuous command sequence AEC -> DUMP is not reliable.
2. This issue will be deferred to the later UART CLI stage.

Reason:
The next AEC target tuning round can proceed using compile-time parameter changes and board reset before each Python DUMP test.
This avoids blocking the project on the current UART command loop issue.
```

### Round 1 conclusion

```
Round 1 provides usable AEC/AGC register observation capability.
The key baseline values for Round 2 are:
0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26

Next step:
Proceed to Round 2 AEC target tuning.
Round 2 should only modify AEC target related registers in small steps and use Python PC Dump summary.csv to compare:
1. mean_brightness
2. highlight_ratio
3. shadow_ratio
4. B/R ratio
5. Laplacian variance
```

## Round 2 - AEC target small-step tuning

### Goal

```
Reduce highlight_ratio from 8.604% to about 3%~5%.
Keep mean_brightness around 90~130.
Avoid obvious shadow_ratio increase.
Keep B/R ratio around 0.9~1.15.
Avoid obvious Laplacian variance decrease.
```

### AEC target levels

| Level | 0x3A0F AEC_WPT | 0x3A10 AEC_BPT | 0x3A1B AEC_WPT2 | 0x3A1E AEC_BPT2 |
| --- | --- | --- | --- | --- |
| baseline | 0x30 | 0x28 | 0x30 | 0x26 |
| minus_1 | 0x2C | 0x24 | 0x2C | 0x22 |
| minus_2 | 0x28 | 0x20 | 0x28 | 0x1E |

### Test method

```
1. Set OV5640_AEC_TUNING_LEVEL in Core/Src/main.c.
2. Compile and download firmware.
3. Reset the board.
4. Run the matching Python PC Dump command directly.
5. Record summary.csv metrics.
6. Repeat for the next level.
```

### Test commands

```bash
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_aec_baseline
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_aec_minus1
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_aec_minus2
```

### Evaluation metrics

```
mean_brightness
highlight_ratio
shadow_ratio
B/R ratio
Laplacian variance
```

### Result table

| Level | mean_brightness | highlight_ratio | shadow_ratio | B/R ratio | Laplacian variance | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | TBD | TBD | TBD | TBD | TBD | 0x30/0x28/0x30/0x26 |
| minus_1 | TBD | TBD | TBD | TBD | TBD | 0x2C/0x24/0x2C/0x22 |
| minus_2 | TBD | TBD | TBD | TBD | TBD | 0x28/0x20/0x28/0x1E |

## Round 2 - AEC target tuning result

### Test purpose

Round 2 is used to verify whether slightly lowering the OV5640 AEC target can reduce the highlight ratio while keeping the overall image brightness and color balance acceptable.

The current baseline AEC target values are from the Round 1 AEC/AGC register dump:

```
0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26
```

The tested AEC target levels are:

```
BASELINE:
0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26

MINUS_1:
0x3A0F = 0x2C
0x3A10 = 0x24
0x3A1B = 0x2C
0x3A1E = 0x22

MINUS_2:
0x3A0F = 0x28
0x3A10 = 0x20
0x3A1B = 0x28
0x3A1E = 0x1E
```

Only `BASELINE` and `MINUS_1` were tested in this round. `MINUS_2` was not tested because `MINUS_1` already caused the mean brightness to drop further.

### Test method

The test uses compile-time AEC target selection instead of UART continuous commands, because Round 1 recorded a known limitation: after sending `AEC\r\n`, the next Python `DUMP\n` may fail to receive the `OV56RGB5` image packet.

For each AEC target level:

```
1. Modify OV5640_AEC_TUNING_LEVEL in Core/Src/main.c.
2. Build the firmware.
3. Download the firmware to the STM32F429 board.
4. Reset the board.
5. Run the Python PC Dump tool directly.
6. Record the generated summary.csv result.
```

Python commands:

```
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_aec_baseline
python tools/pc_dump_rgb565.py --port COM3 --baud 115200 --tag real_aec_minus1
```

### Evaluation criteria

```
Target highlight_ratio: 3% ~ 5%
Target mean_brightness: 90 ~ 130
B/R ratio: 0.9 ~ 1.15
Laplacian variance: no obvious degradation
Shadow ratio: should not increase significantly
```

### Test results

| Index | Time                | Tag               | Image                                              | Mean brightness | Shadow ratio | Highlight ratio | R mean    | G mean    | B mean    | R/G ratio | B/G ratio | B/R ratio | Laplacian variance |
| ----- | ------------------- | ----------------- | -------------------------------------------------- | --------------- | ------------ | --------------- | --------- | --------- | --------- | --------- | --------- | --------- | ------------------ |
| 1     | 2026-07-01T22:31:36 | real_aec_baseline | captures/005_real_aec_baseline_20260701_223136.png | 87.975625       | 14.197917%   | 7.369792%       | 86.096354 | 89.069167 | 87.144740 | 0.966624  | 0.978394  | 1.012177  | 864.019880         |
| 2     | 2026-07-01T22:32:30 | real_aec_minus1   | captures/006_real_aec_minus1_20260701_223230.png   | 82.521250       | 5.729167%    | 6.067708%       | 80.748385 | 82.440208 | 87.279687 | 0.979478  | 1.058703  | 1.080885  | 818.351482         |

### Analysis

Compared with the baseline, `MINUS_1` reduced the highlight ratio:

```
7.369792% -> 6.067708%
```

This shows that lowering the AEC target does have some effect on suppressing overexposed pixels.

However, the mean brightness also dropped:

```
87.975625 -> 82.521250
```

The baseline mean brightness was already lower than the target range of `90 ~ 130`. After applying `MINUS_1`, the image became even darker. Therefore, continuing to `MINUS_2` is not suitable for this scene.

The color balance was still acceptable:

```
B/R ratio:
baseline = 1.012177
minus_1  = 1.080885
```

Both values are within the target range of `0.9 ~ 1.15`, although `MINUS_1` is slightly more blue-biased.

The Laplacian variance decreased slightly:

```
864.019880 -> 818.351482
```

The decrease is not severe, but it indicates that lowering the AEC target does not improve sharpness and may slightly reduce image detail under the current condition.

The shadow ratio result changed from `14.197917%` to `5.729167%`. This is not the typical expected trend when lowering exposure target, so it may be affected by scene difference, camera angle, lighting variation, or AEC convergence timing. Future tests should keep the scene, distance, angle, and lighting more strictly fixed.

### Conclusion

```
1. AEC target MINUS_1 reduced highlight_ratio from 7.369792% to 6.067708%.
2. The reduction was not enough to reach the 3% ~ 5% target range.
3. MINUS_1 also reduced mean_brightness from 87.975625 to 82.521250.
4. Since the baseline mean_brightness was already below the target range of 90 ~ 130, further lowering the AEC target is not suitable.
5. MINUS_2 was not tested in this scene.
6. The current default should remain OV5640_AEC_TARGET_BASELINE.
7. The AEC target tuning interface is useful and can be kept, but MINUS_1 should not be used as the default setting.
```

### Final decision for Round 2

Keep the default AEC target as:

```
#define OV5640_AEC_TUNING_LEVEL       OV5640_AEC_TARGET_BASELINE
```

Current recommended AEC target values:

```
0x3A0F = 0x30
0x3A10 = 0x28
0x3A1B = 0x30
0x3A1E = 0x26
```

Do not continue with `MINUS_2` under the current test condition.

Round 2 can be closed after restoring `OV5640_AEC_TUNING_LEVEL` to `OV5640_AEC_TARGET_BASELINE`.

### Next step

The next stage is AWB / color tuning.

The current color result is acceptable because the B/R ratio stayed within `0.9 ~ 1.15`. Therefore, the AWB stage should also follow a small-step approach:

```
1. First dump current AWB-related registers.
2. Then test only a few fixed AWB modes.
3. Use the same PC Dump summary.csv metrics.
4. Focus on B/R ratio, R/G ratio, B/G ratio, and visual white-balance result.
5. Do not modify brightness, contrast, saturation, or sharpness in the AWB stage.
```