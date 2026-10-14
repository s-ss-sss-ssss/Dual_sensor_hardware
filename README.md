# MagGrad Dual V1

STM32H743 firmware for the dual magnetometer board.

## Build and Flash

```bash
make all
make flash
```

Flashing uses OpenOCD with an ST-Link probe over SWD. The project-level
configuration is `openocd.cfg`; start a debug server without flashing with
`make openocd`.

Serial debug output is sent to both USB CDC and USART1 at 115200 baud.

## Output Formats

The default runtime sensor stream is binary. CSV remains available for debugging
and rate comparisons:

```sh
make
make "EXTRA_DEFS=-DSENSOR_OUTPUT_FORMAT=0"   # CSV mode
```

Binary frame layout:

```text
sync          u8[2]   0xA5 0x5A
version       u8      2
type          u8      0x10=ROUND
round_seq     u32le
tick_us       u32le   Round-start TIM2 counter (1 us/tick)
payload_len   u16le
payload       bytes   Records: type u8, length u16le, body
crc16         u16le   CRC-16/CCITT-FALSE over version..payload
```

The v2 header remains 14 bytes; only the timestamp meaning changes from
milliseconds to microseconds. TIM2 wraps modulo 2^32 about every 71 min 35 s.
Host decoders must use the new unit for this firmware. See the
[communication protocol](doc/host_device_communication_protocol.md).

Record bodies (AK/TMAG/ICM begin with read_done_offset_us u32le):

```text
AK    bus u8, mask u8, hx i16, hy i16, hz i16, status u8, err u8, dor u8
TMAG  ch_mask u8, addr u8, x i16, y i16, z i16, status u8, flags u8
ICM   ax i16, ay i16, az i16, gx i16, gy i16, gz i16, temp i16
PHASE ch1_age_us u32, ch2_age_us u32, ch3_age_us u32
ERR   source u8, code u16, detail u32
STATS ak_frames u32, tmag_frames u32, icm_frames u32, skipped u32, errors u32, device_id u32
```

Use the reader in auto mode for either stream:

```sh
python3 tools/usb_read.py --format auto --duration 10
```

Startup `printf()` text can appear before binary frames. The reader resyncs on
`0xA5 0x5A` and reports skipped non-binary bytes.

Initial binary-mode validation on 2026-05-28:

| Build output | Binary result |
| --- | ---: |
| TMAG only | 253.06 Hz full 12-sensor frames, CRC errors 0, seq jumps 0 |
| AK only | 454.43 Hz full 12-sensor frames, CRC errors 0, seq jumps 0 |
| ICM only | 199.38 Hz data-ready frames, CRC errors 0, seq jumps 0 |
| AK + TMAG | AK 171.21 Hz, TMAG 171.25 Hz, CRC errors 0, seq jumps 0 |

## Magnetometer Initialization Notes

AK09973D and TMAG3001 sensors sit behind TCA9548A I2C multiplexers. A downstream sensor branch can hold SDA low if the previous I2C transaction is interrupted, if a device is left mid-transfer, or if a channel is selected while the downstream device is not ready. When that happens, the upstream bus looks broken even though the TCA and other channels may still be fine.

Observed failure:

```text
TCA select OK, then SDA=0
HAL_I2C_GetError(...) = 0x00000020
subsequent TCA deselect/select calls timeout
```

The fix used in this firmware is:

1. Reset the sensor bus before a full scan.
2. Deselect all TCA channels.
3. Select exactly one TCA channel.
4. Check whether SDA is held low after selecting the channel.
5. If SDA is low, switch SCL/SDA to open-drain GPIO, send clock pulses, generate STOP, reinitialize I2C, reset the bus, and retry the TCA selection.
6. Probe the sensor ID before configuration.
7. Configure the sensor.
8. Deselect the TCA channel before moving to the next channel.

AK09973D is currently initialized with:

```text
CNTL1 = 0x0000
CNTL2 = 0x04
```

That means continuous 10 Hz mode, low-noise drive, high-sensitivity range. The 100 Hz setting previously made debugging less stable, so 10 Hz is the known-good baseline for repeated init/read testing.

TMAG3001 uses the same bus-recovery pattern on I2C3. I2C3 pins are:

```text
SCL = PA8
SDA = PC9
RESET = PC8
```

I2C3 PA8/PC9 must be configured as alternate-function open-drain with internal
pull-ups enabled. On 2026-05-18, TMAG3001 had previously read correctly, then
all downstream devices stopped ACKing after I2C3 GPIO was changed to
`GPIO_NOPULL`. Restoring `GPIO_PULLUP` on both PA8 and PC9 brought the bus back
immediately:

```text
TMAG: TCA OK
TMAG CH1: 3/3 OK
TMAG CH2: 3/3 OK
TMAG CH3: 3/3 OK
TMAG CH4: 3/3 OK
TMAG: first pass 12/12 OK
```

When PA8/PC9 were left as `GPIO_NOPULL`, the debug scan saw no TMAG3001 address
behind any mux channel and TCA select calls started failing. Treat that as an
I2C3 electrical/GPIO configuration failure first, not as a TMAG3001 register
configuration problem.

TMAG3001 is configured from standby, with continuous mode written last:

```text
Device_Config_1 = 0x00   CRC off, Conv_AVG=0 fastest 1x, standard I2C read
Device_Config_2 = 0x00   before configuration
Sensor_Config_1 = 0x70   XYZ magnetic channels enabled
Sensor_Config_2 = 0x00   raw XYZ, default ranges, angle disabled
THR_Config_1..3 = 0x00
Sensor_Config_3 = 0x00
INT_Config_1 = 0x00      interrupts disabled
Sensor_Config_4..6 = 0x00
Device_Config_2 = 0x02   LP_LN=0 low-current, Operating_Mode=2h continuous mode
```

Each configuration write is checked with a single-register readback. Then the
driver writes `Device_Config_2 = 0x02` last and polls `Conv_Status.Result_Status`
before accepting the sensor as initialized. Do not perform a multi-register
configuration readback around TMAG initialization; on the current I2C3 muxed
hardware that caused `CFG READ FAIL` on address `0x34` and destabilized later
channel selects. The runtime firmware initializes AK and TMAG once, then loops
on reads.

`Device_Config_2.LP_LN=1` low-noise mode is optional in the datasheet, but on
the current muxed I2C3 hardware it caused TMAG initialization to fail during
2026-05-18 testing. Keep `LP_LN=0` and `Operating_Mode=2h` for the stable
continuous configuration.

Do not switch TMAG3001 `Device_Config_1.I2C_RD[1:0]` to the direct result-read
mode without a matching recovery path. In testing, direct-read mode caused later
manufacturer-ID register reads to return measurement-frame bytes instead of
`0x5449` until the firmware forced `Device_Config_1 = 0x00` at the start of
initialization. Keep that force-write in place so a previous bad configuration
does not survive a reset line pulse.

For stable high-rate serial output, the TMAG read path keeps the explicit delay
macros at 0 ms and relies on the SCL-pulse recovery path below. With USB CSV
output enabled, 2026-05-28 testing measured 258.84 full 12-sensor TMAG frames
per second using faster I2C3 timing, 1 us recovery-pulse half-cycles, no
inter-sensor guard delay, and no TCA deselect after each channel. A direct
result-read experiment was not accepted: it initialized only 9/12 sensors on the
current board state and did not produce stable output.

AK09973D I2C1 pins are:

```text
SCL = PB8
SDA = PB9
RESET = PB7
```

AK09973D I2C2 pins are:

```text
SCL = PB10
SDA = PB11
RESET = PB12
```

Do not remove the bus-clear path unless the hardware is changed and long-duration serial testing confirms no downstream branch holds SDA low.

## TMAG3001 Clock-Stretching and SCL Pulse Recovery (2026-05-22)

### 问题现象

TMAG3001 在 ~18 Hz 下运行，约 39% 的读取耗时 7–8ms（正常应为 ~250us）。慢读集中在每个 TCA 通道的第 2、3 个传感器（0x35、0x36），第 1 个传感器（0x34）总是快的。

### 根本原因

TMAG3001 工作在 continuous mode，内部 ADC 引擎和 I2C 从机状态机**并行运行**。I2C 规范允许从机在主机读取过程中用 clock stretching（拉低 SCL）来暂停通信，等待内部操作完成。

TCA9548A 切换通道时，会在主总线上产生信号（`START + 0x70 + channel_mask + STOP`）。子总线上的传感器可能把这个事务的部分信号误解为发给自己的 I2C 事务，导致其 I2C 从机状态机进入"等待主机继续"的状态。下一个周期 STM32 再来读该传感器时，传感器立即用 clock stretching 拉住 SCL，HAL 等待 5ms 超时后才放弃，触发重试。

第 1 个传感器（0x34）不受影响，因为它是通道切换后第一个被读取的，此时传感器状态机还没有被后续读取"污染"。

### 为什么 9 个 SCL 脉冲不够

I2C 规范建议 9 个 SCL 脉冲来释放卡住的从机（1 字节 8 bits + 1 ACK bit）。但 TMAG3001 一次读取是 7 个字节（56 data bits + 7 ACK bits = 63 bits）。如果传感器卡在第 1 个字节，需要 63 个脉冲才能把整个事务时钟出去。9 个脉冲只能释放当前字节，传感器仍然认为后续字节还没传完。

### 解决方案

每次 TCA 通道切换后，以及每个传感器读取前，发送 72 个 SCL 脉冲（8 字节 × 9 bits，留有余量）加一个 STOP 条件，强制传感器把它认为"未完成的事务"走完，回到空闲状态。

实现细节（`sensor_tmag3001.c`，`tmag_send_scl_pulses_and_stop()`）：

1. **PE=0**：关闭 I2C3 外设，释放 SCL/SDA 引脚控制权
2. **切换为推挽 GPIO**：I2C 外设使用开漏输出，无法对抗传感器的下拉。推挽输出可以强制拉高 SCL，即使传感器在 clock stretching
3. **发送 N 个 SCL 脉冲**：当前默认 1us 半周期，传感器通过 TCA 子总线看到这些脉冲
4. **发送 STOP 条件**：SCL 高时 SDA 低→高，传感器状态机复位到空闲
5. **恢复为 AF 开漏 GPIO**：重新交给 I2C3 外设
6. **PE=1**：重新使能 I2C3
7. **ANFOFF errata**：STM32H7 模拟滤波器可能在复位后产生虚假 BUSY，用 PE=0/ANFOFF/PE=1 序列清除

脉冲数量：通道级恢复用 72 个，传感器级恢复用 18 个（0x36 地址用 36 个，因为其 A2 引脚接 SDA，更容易卡住）。

### 结果

修复后内部读路径曾达到 113 Hz，0% fail rate，0% 慢读（>2ms），每次读取
avg=242us。2026-05-28 进一步优化 I2C3 timing 和恢复脉冲后，端到端 USB CSV
输出长测达到 258.84 Hz 全 12 颗/轮。

## TMAG3001 High-Rate Tuning (2026-05-28)

The accepted default TMAG-only configuration is:

```text
CubeMX I2C3.Timing = 0x00200408
TMAG_PULSE_HALF_US = 1
TMAG_GUARD_US = 0
TMAG_RESELECT_AFTER_SENSOR_PULSE = 0
TMAG_SKIP_GND_SENSOR_PULSE = 1
TMAG_DESELECT_AFTER_CHANNEL = 0
```

Measured steady TMAG-only USB CSV rates:

| Configuration | Full 12-sensor frame Hz | Result |
| --- | ---: | --- |
| Old 400 kHz timing, 150 us guard | ~70 | stable baseline |
| Fast I2C3 timing, 30 us guard, keep channel deselect | 119.16 | stable, 20 s |
| Fast I2C3 timing, 30 us guard, no channel deselect | 119.92 | stable, 60 s |
| Fast I2C3 timing, 4 us pulse half-cycle, no guard | 144.12 | stable short test |
| Fast I2C3 timing, 3 us pulse half-cycle, no guard | 169.63 | stable short test |
| Fast I2C3 timing, 2 us pulse half-cycle, no guard | 205.56 | stable short test |
| Fast I2C3 timing, 1 us pulse half-cycle, no guard | 258.84 | stable, 60 s |
| No recovery pulses | 36.48 | rejected, uneven sensor counts |
| 0 us pulse half-cycle | 45.30 | rejected, ineffective recovery |

The high-rate result depends on keeping the recovery pulses, just making them
shorter. Removing the pulses makes the read loop slower because the hidden
recovery/retry path dominates.

## AK09973D I2C1/I2C2 High-Rate Tuning (2026-05-28)

I2C1 and I2C2 use the same faster timing value as I2C3 by default:

```text
CubeMX I2C1.Timing = 0x00200408
CubeMX I2C2.Timing = 0x00200408
```

AK-only USB CSV measurements:

| Configuration | AK full-frame Hz | Result |
| --- | ---: | --- |
| Original I2C1/I2C2 timing `0x00901227` | 243.73 | stable short test |
| Only I2C2 at `0x00200408` | 322.31 | stable short test |
| Only I2C1 at `0x00200408` | 322.80 | stable short test |
| I2C1 + I2C2 at `0x00200408` | 476.69 | stable, 60 s |
| I2C1 + I2C2 at `0x00100309` | 476.76 | stable short test |

The accepted default is `0x00200408` on both AK buses because it matches the
TMAG/I2C3 timing and passed the 60 s AK-only test with 12/12 sensors, 0 bad CSV
lines, and all AK error fields equal to 0. AK + TMAG together measured about
168.3 Hz for both sensor families over USB CSV with 0 bad lines.

## End-to-End USB Rate Measurements (2026-05-27)

Measured by building each output combination with `EXTRA_DEFS`, flashing through
SWD, then reading `/dev/cu.usbmodem*` for 6 seconds. These are end-to-end USB
CSV output rates, not only sensor bus transaction rates.

| Build output | AK full-frame Hz | TMAG full-frame Hz | ICM line Hz |
| --- | ---: | ---: | ---: |
| ICM only | - | - | 8911.78 |
| AK only | 244.39 | - | - |
| TMAG only | - | 70.42 | - |
| AK + ICM | 236.81 | - | 236.78 |
| TMAG + ICM | - | 69.90 | 69.82 |
| AK + TMAG | 54.64 | 54.65 | - |
| AK + TMAG + ICM | 54.46 | 54.49 | 54.49 |

For repeatable combination tests without editing `main.c`, pass output macros
through `EXTRA_DEFS`, for example:

```sh
make clean
make "EXTRA_DEFS=-DSENSOR_OUTPUT_ICM=1 -DSENSOR_OUTPUT_AK=0 -DSENSOR_OUTPUT_TMAG=1"
```
