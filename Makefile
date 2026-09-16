#==============================================================================
# STM32H743VI Makefile — 完全 VS Code 构建
#==============================================================================

# 目标芯片
TARGET = MagGrad_Dual_V1
MCU    = STM32H743VITx

# 工具链
PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc -x assembler-with-cpp
CP      = $(PREFIX)objcopy
SZ      = $(PREFIX)size
OD      = $(PREFIX)objdump
OPENOCD ?= openocd
OPENOCD_CFG ?= openocd.cfg

# 构建输出
BUILD_DIR = build

#==============================================================================
# 源文件
#==============================================================================
# 应用代码
APP_SRC  = Core/Src/main.c \
            Core/Src/gpio.c \
            Core/Src/dma.c \
            Core/Src/i2c.c \
            Core/Src/i2c_recovery.c \
            Core/Src/spi.c \
            Core/Src/tim.c \
            Core/Src/usart.c \
            Core/Src/stm32h7xx_it.c \
            Core/Src/stm32h7xx_hal_msp.c \
            Core/Src/icm42670.c \
            Core/Src/ak09973d.c \
            Core/Src/tmag3001.c \
            Core/Src/csv_writer.c \
            Core/Src/binary_writer.c \
            Core/Src/sensor_ak09973d.c \
            Core/Src/sensor_tmag3001.c

# USB 设备
USB_SRC  = USB_DEVICE/App/usb_device.c \
           USB_DEVICE/Target/usbd_conf.c \
           USB_DEVICE/App/usbd_desc.c \
           USB_DEVICE/App/usbd_cdc_if.c

# HAL 驱动
HAL_SRC  = Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pcd.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pcd_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_usb.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hsem.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim_ex.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c \
            Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart_ex.c

# USB Middleware
USB_MW_SRC = Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
             Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
             Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
             Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c

# CMSIS / System
SYS_SRC  = Core/Src/system_stm32h7xx.c
SYS_SRC += Core/Src/syscalls.c
SYS_SRC += Core/Src/sysmem.c

# Startup
STARTUP_SRC = Core/Startup/startup_stm32h743vitx.s

# 所有 C 源文件
C_SOURCES = $(APP_SRC) $(USB_SRC) $(HAL_SRC) $(USB_MW_SRC) $(SYS_SRC)

# 所有汇编文件
AS_SOURCES = $(STARTUP_SRC)

#==============================================================================
# 包含路径
#==============================================================================
IPATH  = -ICore/Inc
IPATH += -IUSB_DEVICE/App
IPATH += -IUSB_DEVICE/Target
IPATH += -IDrivers/STM32H7xx_HAL_Driver/Inc
IPATH += -IDrivers/STM32H7xx_HAL_Driver/Inc/Legacy
IPATH += -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc
IPATH += -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
IPATH += -IDrivers/CMSIS/Device/ST/STM32H7xx/Include
IPATH += -IDrivers/CMSIS/Include

#==============================================================================
# 宏定义
#==============================================================================
DEFS  = -DUSE_PWR_LDO_SUPPLY
DEFS += -DUSE_HAL_DRIVER
DEFS += -DSTM32H743xx

#==============================================================================
# 编译选项
#==============================================================================
# CPU = Cortex-M7, FPU = FPv5-D16, 优化 -Og (调试用)
OPT  = -Og
COPT = -Wall -fdata-sections -ffunction-sections

# CPU / FPU 参数 — Cortex-M7
MCU_FLAGS = -mcpu=cortex-m7 -mthumb
FP_FLAGS  = -mfpu=fpv5-d16 -mfloat-abi=hard

# C 编译
CFLAGS  = $(OPT) $(MCU_FLAGS) $(FP_FLAGS) -std=c11 -c
CFLAGS += $(DEFS)
CFLAGS += $(EXTRA_DEFS)
CFLAGS += $(IPATH)
CFLAGS += -MD -MP -MF"$(@:%.o=%.d)"

# 汇编编译
ASFLAGS  = -c $(MCU_FLAGS) $(FP_FLAGS) -x assembler-with-cpp
ASFLAGS += $(DEFS)
ASFLAGS += $(IPATH)

# 链接
LDFLAGS  = $(MCU_FLAGS) $(FP_FLAGS) -specs=nosys.specs -specs=nano.specs
LDFLAGS += -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += -T STM32H743VITX_FLASH.ld

#==============================================================================
# 对象文件
#==============================================================================
C_OBJ  = $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o))
AS_OBJ = $(addprefix $(BUILD_DIR)/,$(STARTUP_SRC:.s=.o))
OBJ    = $(C_OBJ) $(AS_OBJ)

#==============================================================================
# 目标
#==============================================================================
ELF    = $(BUILD_DIR)/$(TARGET).elf
BIN    = $(BUILD_DIR)/$(TARGET).bin
HEX    = $(BUILD_DIR)/$(TARGET).hex
LST    = $(BUILD_DIR)/$(TARGET).lst

.PHONY: all clean flash debug openocd

all: $(BUILD_DIR) $(ELF) $(BIN) $(HEX) $(LST)
	@echo "=== Build complete ==="
	@$(SZ) $(ELF)

$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

# 从 .c -> .o
$(BUILD_DIR)/%.o: %.c
	@if not exist "$(subst /,\,$(dir $@))" mkdir "$(subst /,\,$(dir $@))"
	$(CC) $(CFLAGS) -o $@ $<

# 从 .s -> .o
$(BUILD_DIR)/%.o: %.s
	@if not exist "$(subst /,\,$(dir $@))" mkdir "$(subst /,\,$(dir $@))"
	$(AS) $(ASFLAGS) -o $@ $<

$(ELF): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@
	$(OD) -h $@ > $(LST)

$(BIN): $(ELF)
	$(CP) -O binary $< $@

$(HEX): $(ELF)
	$(CP) -O ihex $< $@

clean:
	@if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"

#==============================================================================
# 烧录 (OpenOCD + ST-Link)
#==============================================================================
flash: $(ELF)
	$(OPENOCD) -f $(OPENOCD_CFG) \
		-c "program $(ELF) verify reset exit"

#==============================================================================
# 调试 (OpenOCD + GDB)
#==============================================================================
debug: $(ELF)
	$(OPENOCD) -f $(OPENOCD_CFG) \
		-c "program $(ELF) verify reset exit" &

#==============================================================================
# OpenOCD 调试服务器（单独运行）
#==============================================================================
openocd:
	$(OPENOCD) -f $(OPENOCD_CFG)

#==============================================================================
# 使用 make debug 烧录并进入调试
# - 启动 OpenOCD 服务端（后台）
# - 等待 2 秒让 OpenOCD 启动
# - 启动 arm-none-eabi-gdb 连接 OpenOCD
#==============================================================================
debug-gdb: $(ELF)
	@echo "Starting OpenOCD server..."
	@$(OPENOCD) -f $(OPENOCD_CFG) &
	@sleep 2
	@echo "Starting GDB..."
	arm-none-eabi-gdb -iex "target remote localhost:3333" \
		$(ELF)
