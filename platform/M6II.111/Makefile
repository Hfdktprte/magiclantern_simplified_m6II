MODEL = M6II
FW_VERSION = 111

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR  = M6200111.FIR
FIRMWARE_ID = 0x00000811
MAIN_FIRMWARE_ADDR = 0xE0040000

# user_mem_start from cstart struct. Align to 0x10 up
RESTARTSTART = 0xb8d20 # 0xb0078 in ROM

# Cortex A9, binaries loaded as Thumb
# RAW_LV_EDMAC_CHANNEL_ADDR now lives in consts.h, as in Bilal's M50 port.
# Keep bpp fixed at native 14-bit until the M6II PackMode field is traced.
CFLAGS_PLATFORM = -mthumb -mlong-calls \
    -DSHAD_GAIN_REGISTER=0u \
    -DPACK32_MODE=0u \
    -DMODE_10BIT=0u -DMODE_12BIT=0u -DMODE_14BIT=0u -DMODE_16BIT=0u
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
# Match Bilal's M50 integration: the D8 memcpy compatibility backend must be
# linked into the core so mlv_lite.mo can resolve its exported symbols.
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o edmac-d8.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/test_features.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_state_probe.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_dump_probe.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_burst_rec.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_ram_mlv_rec.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/edmac_d8_test.o

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
