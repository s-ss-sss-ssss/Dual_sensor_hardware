# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Build Commands

```bash
make all      # Build firmware (ELF, BIN, HEX outputs to build/)
make clean    # Clean build artifacts
make flash   # Flash firmware via ST-Link (st-flash)
make debug   # Flash and debug with OpenOCD
make openocd # Start OpenOCD server (run separately before debugging)
```

**VSCode tasks**: `make all` (default), `make clean`, `flash`, `openocd server`
**VSCode debug**: "Debug with ST-Link (OpenOCD)" - preLaunchTask builds before debug

**Toolchain**: ARM GNU Toolchain (arm-none-eabi-gcc). Configured for Cortex-M7 with FPU (FPv5-D16, hard float ABI).

## Hardware Architecture

### MCU: STM32H743VITx
- 480MHz Cortex-M7, 2MB Flash, 1MB SRAM
- MPU configured with tight memory protection

### Communication Buses

| Bus   | Purpose                    | Sensors                         |
|-------|---------------------------|---------------------------------|
| I2C1  | Magnetometers (AK09973D)  | 6 channels via TCA9548A         |
| I2C2  | Magnetometers (AK09973D)  | 6 channels via TCA9548A         |
| I2C3  | 3D Magnetometers (TMAG)  | 4 channels via TCA9548A (12 sensors) |
| SPI1  | IMU (ICM42670)            | Direct CS pin (PB0)             |
| USB   | CDC virtual serial port   | Communication/debug output       |

### TCA9548A I2C Mux
- Address: 0x70 (7-bit)
- 8 channels (bit mask selects channel)
- Each TCA channel connects to one or more sensors
- See `tca9548.h` for `TCA9548_Select()` helper

### Sensor Configuration
- **AK09973D**: 12 total magnetometers (6 per I2C bus), address 0x10
- **TMAG3001**: 4 total (one sensor per channel), address 0x34 (A2=GND)
- **ICM42670**: Single IMU on SPI1

### I2C Reset Lines
```
I2C1_RESET: PB7
I2C2_RESET: PB12
I2C3_RESET: PC8
```

## Code Architecture

### Directory Structure
```
Core/Src/           # Application code
  main.c            # Entry point, initialization, main loop
  i2c.c             # I2C1/I2C2/I2C3 peripheral init + HAL MSP
  sensor_*.c        # Sensor abstraction layer
  ak09973d.c        # AK09973D register-level driver
  tmag3001.c        # TMAG3001 register-level driver
  icm42670.c       # ICM42670 SPI driver
Core/Inc/           # Header files
Drivers/            # STM32 HAL and CMSIS
USB_DEVICE/         # USB CDC implementation
Middlewares/        # USB device library
```

### Sensor Driver Layer
1. **Register-level drivers** (`ak09973d.c`, `tmag3001.c`, `icm42670.c`): Direct register I/O
2. **Abstraction layer** (`sensor_*.c`): Handles TCA muxing, multi-instance management, CSV output

### USB CDC Communication
- `CDC_Transmit_FS()` sends data to host
- `printf()` redirected to USB CDC via `_write()` override
- `USB_Send_String()` as convenience wrapper
- Buffer sizes in `usbd_cdc_if.h`: `APP_TX_DATA_SIZE`, `APP_RX_DATA_SIZE`

### Key Patterns
- Sensor instances stored in static arrays (`g_ak_list[]`, `g_tmag_list[]`)
- All sensors initialize once in `main()` startup
- `TMAG3001_ACTIVE_TCA_MASK` controls which TCA channels are scanned
- All CSV output format: `SENSOR_TYPE,channel,misc,data...`

## Development Notes

### Adding New Sensors
1. Create register-level driver (see `ak09973d.c` as template)
2. Add abstraction layer with TCA mux support (`sensor_*.c`)
3. Declare public API in header (`sensor_*.h`)
4. Call init function from `main.c` in USER CODE BEGIN 2

### I2C Speed Notes
- I2C3 runs at 100kHz (required by TMAG3001)
- I2C1/I2C2 run at 400kHz (for AK09973D)

### Regenerating from STM32CubeMX
The `.ioc` file is the STM32CubeMX project. If modified:
1. Open in STM32CubeMX
2. Regenerate code (replaces Core/Src/Inc peripheral files)
3. **Do not overwrite** custom sensor drivers - they use USER CODE markers

### Build Output Location
All outputs in `build/` directory:
- `MagGrad_Dual_V1.elf` - ELF for debugging
- `MagGrad_Dual_V1.bin` - Raw binary for flashing
- `MagGrad_Dual_V1.hex` - HEX for programming tools
- `MagGrad_Dual_V1.map` - Linker map (symbol sizes, memory usage)

## Hardware Schematics Reference
Datasheets in `doc/`:
- AK09973D (magnetometer)
- TMAG3001 (3D magnetometer with angle)
- ICM42670 (6-axis IMU)
