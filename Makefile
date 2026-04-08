# Reproducer Makefile -- builds STM32CubeU5's unmodified
# Nx_TCP_Echo_Server sample with arm-none-eabi-gcc.
#
# The only "modification" is that we provide our own mx_wifi_conf.h via a
# higher-priority -I path so the sample picks up real WiFi credentials
# from inc/wifi_secrets.h (gitignored) instead of the YOUR_SSID
# placeholder shipped with the sample.  All other source files come
# straight from the STM32CubeU5 tree.
#
# Usage:
#   1. Set STM32CUBE_DIR if your CubeU5 install is not at the default.
#   2. Create inc/wifi_secrets.h with your WIFI_SSID / WIFI_PASSWORD.
#   3. make
#   4. make flash
#   5. python3 client.py <mcu-ip> 6000

STM32CUBE_DIR ?= $(HOME)/code/stm32/STM32CubeU5

SAMPLE := $(STM32CUBE_DIR)/Projects/B-U585I-IOT02A/Applications/NetXDuo/Nx_TCP_Echo_Server

HAL_DIR     := $(STM32CUBE_DIR)/Drivers/STM32U5xx_HAL_Driver
CMSIS_DIR   := $(STM32CUBE_DIR)/Drivers/CMSIS
THREADX_DIR := $(STM32CUBE_DIR)/Middlewares/ST/threadx
NETXDUO_DIR := $(STM32CUBE_DIR)/Middlewares/ST/netxduo
MXWIFI_DIR  := $(STM32CUBE_DIR)/Drivers/BSP/Components/mx_wifi

STARTUP    := $(SAMPLE)/STM32CubeIDE/Application/User/Startup/startup_stm32u585aiixq.s
LDSCRIPT   := $(SAMPLE)/STM32CubeIDE/STM32U585AIIXQ_FLASH.ld

PROGRAMMER ?= /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI

# ── Toolchain ─────────────────────────────────────────────────────

PREFIX := arm-none-eabi-
CC     := $(PREFIX)gcc
AS     := $(PREFIX)gcc
LD     := $(PREFIX)gcc
OBJCPY := $(PREFIX)objcopy
SIZE   := $(PREFIX)size

MCU := -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard

DEFS := \
	-DSTM32U585xx \
	-DUSE_HAL_DRIVER \
	-DUSE_FULL_ASSERT \
	-DTX_INCLUDE_USER_DEFINE_FILE \
	-DNX_INCLUDE_USER_DEFINE_FILE \
	-DNX_DRIVER_DEFERRED_PROCESSING \
	-DNX_PACKET_HEADER_PAD \
	-DNX_PACKET_HEADER_PAD_SIZE=4

# Include paths.  Order matters: our local inc/ comes FIRST so the
# sample picks up our copy of mx_wifi_conf.h (with credentials) before
# its own placeholder copy.
INCS := \
	-Iinc \
	-I$(SAMPLE)/Core/Inc \
	-I$(SAMPLE)/NetXDuo/App \
	-I$(SAMPLE)/NetXDuo/Target \
	-I$(SAMPLE)/AZURE_RTOS/App \
	-I$(HAL_DIR)/Inc \
	-I$(CMSIS_DIR)/Device/ST/STM32U5xx/Include \
	-I$(CMSIS_DIR)/Include \
	-I$(THREADX_DIR)/common/inc \
	-I$(THREADX_DIR)/ports/cortex_m33/gnu/inc \
	-I$(NETXDUO_DIR)/common/inc \
	-I$(NETXDUO_DIR)/ports/cortex_m33/gnu/inc \
	-I$(NETXDUO_DIR)/addons/dhcp \
	-I$(NETXDUO_DIR)/common/drivers/wifi/mxchip \
	-I$(MXWIFI_DIR) \
	-I$(MXWIFI_DIR)/core \
	-I$(MXWIFI_DIR)/io_pattern

# ── Source files (all unmodified ST sample sources) ───────────────

APP_SRCS := \
	$(SAMPLE)/Core/Src/main.c \
	$(SAMPLE)/Core/Src/stm32u5xx_it.c \
	$(SAMPLE)/Core/Src/stm32u5xx_hal_msp.c \
	$(SAMPLE)/Core/Src/stm32u5xx_hal_timebase_tim.c \
	$(SAMPLE)/Core/Src/system_stm32u5xx.c \
	$(SAMPLE)/Core/Src/app_threadx.c \
	$(SAMPLE)/AZURE_RTOS/App/app_azure_rtos.c \
	$(SAMPLE)/NetXDuo/App/app_netxduo.c \
	$(SAMPLE)/STM32CubeIDE/Application/User/Core/syscalls.c \
	$(SAMPLE)/STM32CubeIDE/Application/User/Core/sysmem.c

HAL_SRCS := \
	$(HAL_DIR)/Src/stm32u5xx_hal.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_cortex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_rcc.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_rcc_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_pwr.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_pwr_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_gpio.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_dma.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_dma_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_flash.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_flash_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_icache.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_exti.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_tim.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_tim_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_uart.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_uart_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_spi.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_spi_ex.c \
	$(HAL_DIR)/Src/stm32u5xx_hal_rng.c

THREADX_C_SRCS := $(wildcard $(THREADX_DIR)/common/src/*.c)

THREADX_ASM_SRCS := \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_initialize_low_level.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_context_restore.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_context_save.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_interrupt_control.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_interrupt_disable.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_interrupt_restore.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_schedule.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_stack_build.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_system_return.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_timer_interrupt.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_secure_stack_allocate.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_secure_stack_free.S \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_secure_stack_initialize.S

THREADX_PORT_C_SRCS := \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/tx_thread_secure_stack.c \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/txe_thread_secure_stack_allocate.c \
	$(THREADX_DIR)/ports/cortex_m33/gnu/src/txe_thread_secure_stack_free.c

NETXDUO_SRCS := \
	$(wildcard $(NETXDUO_DIR)/common/src/*.c) \
	$(wildcard $(NETXDUO_DIR)/addons/dhcp/*.c) \
	$(NETXDUO_DIR)/common/drivers/wifi/mxchip/nx_driver_emw3080.c \
	$(NETXDUO_DIR)/common/drivers/wifi/mxchip/mx_wifi_azure_rtos.c

MXWIFI_SRCS := \
	$(MXWIFI_DIR)/mx_wifi.c \
	$(MXWIFI_DIR)/core/mx_address.c \
	$(MXWIFI_DIR)/core/mx_wifi_hci.c \
	$(MXWIFI_DIR)/core/mx_wifi_ipc.c \
	$(MXWIFI_DIR)/core/mx_wifi_slip.c \
	$(MXWIFI_DIR)/core/checksumutils.c \
	$(MXWIFI_DIR)/io_pattern/mx_wifi_spi.c

ALL_C_SRCS := $(APP_SRCS) $(HAL_SRCS) $(THREADX_C_SRCS) \
              $(THREADX_PORT_C_SRCS) $(NETXDUO_SRCS) $(MXWIFI_SRCS)
ALL_ASM_SRCS := $(THREADX_ASM_SRCS)

# Local sources (built from this repo, not from the ST tree).
# These must come FIRST in the link order so --allow-multiple-definition
# picks our HardFault/BusFault/MemManage/UsageFault handlers over the
# silent infinite loops the sample defines in stm32u5xx_it.c.
LOCAL_C_SRCS := src/io_putchar.c src/fault_dump.c src/nx_tcp_fast_periodic_processing_local.c src/nx_tcp_server_socket_accept_local.c src/watchdog.c src/syn_wrap.c

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/repro

LOCAL_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/local/%.o,$(LOCAL_C_SRCS))
C_OBJS := $(patsubst /%,$(BUILD_DIR)/%,$(patsubst %.c,%.o,$(ALL_C_SRCS)))
ASM_OBJS := $(patsubst /%,$(BUILD_DIR)/%,$(patsubst %.S,%.o,$(ALL_ASM_SRCS)))
OBJS := $(LOCAL_OBJS) $(C_OBJS) $(ASM_OBJS) $(BUILD_DIR)/startup.o

CFLAGS := $(MCU) -std=gnu11 -g3 -O2 \
	-ffunction-sections -fdata-sections \
	-Wall -Wno-format -Wno-unused-parameter \
	$(DEFS) $(INCS) \
	--specs=nano.specs

# Compile only the sample's app_netxduo.c with our PRINT_DATA override
# pre-included.  Applying -include globally would break NetXDuo's own
# middleware sources that also pull in nx_api.h with their own setup.
APP_NETXDUO_OBJ := $(patsubst /%,$(BUILD_DIR)/%,$(patsubst %.c,%.o,$(SAMPLE)/NetXDuo/App/app_netxduo.c))
$(APP_NETXDUO_OBJ): CFLAGS += -include $(CURDIR)/inc/app_netxduo.h

ASFLAGS := $(MCU) -g3 $(DEFS) $(INCS)

LDFLAGS := $(MCU) \
	-T$(LDSCRIPT) \
	--specs=nano.specs --specs=nosys.specs \
	-Wl,--gc-sections \
	-Wl,--allow-multiple-definition \
	-Wl,--defsym=__RAM_segment_used_end__=_end \
	-Wl,--defsym=_vectors=g_pfnVectors \
	-Wl,--wrap=_nx_tcp_packet_send_syn \
	-Wl,--wrap=_nx_tcp_packet_process \
	-Wl,--wrap=_nx_tcp_server_socket_accept \
	-Wl,-Map=$(TARGET).map \
	-lc -lm -lnosys

.PHONY: all clean flash size

all: $(TARGET).elf size

$(BUILD_DIR)/%.o: /%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: /%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/startup.o: $(STARTUP)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/local/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(LD) $(OBJS) $(LDFLAGS) -o $@
	$(OBJCPY) -O binary $@ $(TARGET).bin

size: $(TARGET).elf
	$(SIZE) $<

flash: $(TARGET).elf
	sudo $(PROGRAMMER) -c port=SWD freq=1000 reset=SWrst -w $< -rst

clean:
	rm -rf $(BUILD_DIR)
