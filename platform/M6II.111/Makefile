MODEL = M6II
FW_VERSION = 111

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR  = M6200111.FIR
FIRMWARE_ID = 0x00000811
# ML boot/link firmware entry base. This is NOT the ROM0 dump base.
# M6II boots Canon firmware at 0xE0040000; ROM analysis may use a 0xE0000000 image base.
MAIN_FIRMWARE_ADDR = 0xE0040000

# user_mem_start from cstart struct. Align to 0x10 up
RESTARTSTART = 0xb8d20 # 0xb0078 in ROM

# Cortex A9, binaries loaded as Thumb
CFLAGS_PLATFORM = -mthumb -mlong-calls \
    -DSHAD_GAIN_REGISTER=0u
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
# Match Bilal 32dbd9c integration: platform overrides + D8 EDMAC backend.
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o edmac-d8.o
# M6II-only Debug menu tool for enumerating Canon RAW LiveView geometries.
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/m6ii_raw_geometry.o

include ../Makefile

# Bilal's raw.c needs the M6II camera-specific RAW metadata definitions.
$(BUILD_DIR)/raw.o: CFLAGS += -include raw_compat.h

.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
