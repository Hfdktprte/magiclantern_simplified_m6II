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
# Bilal-style D8 RAW video uses RAW_LV_EDMAC_CHANNEL_ADDR directly from raw.c.
# M6II ROM: RAW destination channel 0x4B -> MMIO block 0xD04C0300.
# Keep bpp fixed at native 14-bit for first mlv_lite bring-up; do not touch an
# unverified D8 PACK32 register. shamem_read() currently returns 0, so the
# generic request path becomes a no-op when mlv_lite requests 14-bit.
# SHAD_GAIN_REGISTER is likewise only a compile-time placeholder while the
# M6II shamem_read override is a no-op; do not borrow M50's unverified address.
CFLAGS_PLATFORM = -mthumb -mlong-calls \
    -DRAW_LV_EDMAC_CHANNEL_ADDR=0xD04C0300u \
    -DSHAD_GAIN_REGISTER=0u \
    -DPACK32_MODE=0u \
    -DMODE_10BIT=0u -DMODE_12BIT=0u -DMODE_14BIT=0u -DMODE_16BIT=0u
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/test_features.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_state_probe.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_dump_probe.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_burst_rec.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_ram_mlv_rec.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/edmac_d8_test.o

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
