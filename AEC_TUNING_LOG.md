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