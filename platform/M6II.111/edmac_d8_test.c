/** \file
 * EOS M6 Mark II DIGIC 8 EDMAC RAM-to-RAM bring-up test.
 *
 * Uses Canon's own M6II MemoryToMemoryEsub5 wrappers recovered from ROM.
 * This deliberately copies a known 1 MiB pattern between two DMA buffers.
 * It does not touch the live RAW writer channel and does not redirect LiveView.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <edmac.h>
#include <timer.h>

#define M6II_EDMAC_TEST_SIZE       (1024 * 1024)
#define M6II_EDMAC_ROW_BYTES       4096
#define M6II_EDMAC_ROWS            (M6II_EDMAC_TEST_SIZE / M6II_EDMAC_ROW_BYTES)
#define M6II_EDMAC_RD_CH            58
#define M6II_EDMAC_WR_CH            23
#define M6II_EDMAC_MODE             0
#define M6II_EDMAC_LOG              "M6II_EDMAC_TEST.TXT"

/*
 * M6II ROM 1.1.1 / internal 5.9.2, Canon MemoryToMemoryEsub5 helpers.
 * These wrappers keep Canon's own channel/Boomer setup logic in control.
 * Thumb function pointers must have bit 0 set.
 */
typedef void (*m6ii_esub5_setup_fn)(const uint32_t *channels);
typedef void (*m6ii_esub5_void_fn)(void);
typedef void (*m6ii_esub5_addr_fn)(const uint32_t *addresses);
typedef void (*m6ii_esub5_size_fn)(const uintptr_t *args);

#define M6II_ESUB5_SETUP       ((m6ii_esub5_setup_fn)(0xE0679898u | 1u))
#define M6II_ESUB5_CLEANUP     ((m6ii_esub5_void_fn)(0xE0679922u | 1u))
#define M6II_ESUB5_START       ((m6ii_esub5_void_fn)(0xE0679952u | 1u))
#define M6II_ESUB5_SET_ADDR    ((m6ii_esub5_addr_fn)(0xE067996Cu | 1u))
#define M6II_ESUB5_SET_SIZE    ((m6ii_esub5_size_fn)(0xE0679986u | 1u))

extern void *_alloc_dma_memory(size_t size);
extern void _free_dma_memory(void *ptr);
extern void PwrMng_WakeSubChips(const uint32_t *list);
extern void PwrMng_SuspendSubChips(const uint32_t *list);

static volatile int m6ii_edmac_busy = 0;
/* EsubN maps to power-manager subchip N-1 on DIGIC 8; terminate with 7. */
static const uint32_t m6ii_esub5_devices[] = {4, 7};

static uint32_t m6ii_checksum32(const volatile uint32_t *p, uint32_t bytes)
{
    uint32_t sum = 0;
    uint32_t words = bytes / 4;
    for (uint32_t i = 0; i < words; i++)
        sum = (sum << 5) - sum + p[i];
    return sum;
}

static void m6ii_edmac_test_run()
{
    if (m6ii_edmac_busy)
    {
        NotifyBox(2000, "EDMAC test already running");
        return;
    }
    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_edmac_busy = 1;
    volatile uint32_t *src = 0;
    volatile uint32_t *dst = 0;
    int stage = 0;
    int tail_seen = 0;
    int domain_awake = 0;
    int esub_setup = 0;
    uint32_t mismatches = 0;
    uint32_t first_mismatch = 0xffffffff;
    uint32_t checksum_src = 0;
    uint32_t checksum_dst = 0;
    uint64_t elapsed_us = 0;

    src = (volatile uint32_t *)_alloc_dma_memory(M6II_EDMAC_TEST_SIZE);
    dst = (volatile uint32_t *)_alloc_dma_memory(M6II_EDMAC_TEST_SIZE);
    if (!src || !dst)
    {
        stage = -1;
        goto cleanup;
    }

    const uint32_t words = M6II_EDMAC_TEST_SIZE / 4;
    for (uint32_t i = 0; i < words; i++)
    {
        src[i] = 0x6d360000u ^ (i * 0x10204081u);
        dst[i] = 0;
    }
    src[words - 1] = 0x4d36444d; /* M6DM */
    dst[words - 1] = 0;
    checksum_src = m6ii_checksum32(src, M6II_EDMAC_TEST_SIZE);
    stage = 1;

    /* D8 EDMAC accesses hard-lock if the corresponding subchip is asleep. */
    PwrMng_WakeSubChips(m6ii_esub5_devices);
    domain_awake = 1;
    stage = 2;

    /* Canon's Esub5 setup selects the M6II-native Boomer config for channel 58. */
    const uint32_t channels[2] = { M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH };
    M6II_ESUB5_SETUP(channels);
    esub_setup = 1;
    stage = 3;

    struct edmac_info src_info;
    struct edmac_info dst_info;
    memset(&src_info, 0, sizeof(src_info));
    memset(&dst_info, 0, sizeof(dst_info));
    src_info.xb = M6II_EDMAC_ROW_BYTES;
    src_info.yb = M6II_EDMAC_ROWS - 1;
    dst_info.xb = M6II_EDMAC_ROW_BYTES;
    dst_info.yb = M6II_EDMAC_ROWS - 1;

    const uint32_t addresses[2] = { (uint32_t)src, (uint32_t)dst };
    const uintptr_t size_args[3] = {
        (uintptr_t)&src_info,
        (uintptr_t)&dst_info,
        M6II_EDMAC_MODE
    };
    M6II_ESUB5_SET_ADDR(addresses);
    M6II_ESUB5_SET_SIZE(size_args);
    stage = 4;

    uint64_t t0 = get_us_clock();
    M6II_ESUB5_START();

    /* Poll DMA memory only; no EDMAC MMIO reads. */
    for (int i = 0; i < 200; i++)
    {
        if (dst[words - 1] == src[words - 1])
        {
            tail_seen = 1;
            break;
        }
        msleep(1);
    }
    elapsed_us = get_us_clock() - t0;
    stage = 5;

    M6II_ESUB5_CLEANUP();
    esub_setup = 0;
    PwrMng_SuspendSubChips(m6ii_esub5_devices);
    domain_awake = 0;
    stage = 6;

    checksum_dst = m6ii_checksum32(dst, M6II_EDMAC_TEST_SIZE);
    for (uint32_t i = 0; i < words; i++)
    {
        if (src[i] != dst[i])
        {
            if (first_mismatch == 0xffffffff)
                first_mismatch = i * 4;
            mismatches++;
        }
    }
    stage = 7;

cleanup:
    if (esub_setup)
        M6II_ESUB5_CLEANUP();
    if (domain_awake)
        PwrMng_SuspendSubChips(m6ii_esub5_devices);

    FILE *f = FIO_CreateFile(M6II_EDMAC_LOG);
    if (f)
    {
        char text[768];
        int len = snprintf(text, sizeof(text),
            "M6II DIGIC8 native Esub5 EDMAC copy v2\n"
            "size=%u\nrows=%u\nrow_bytes=%u\n"
            "rd_channel=%u\nwr_channel=%u\npower_subchip=4\n"
            "src=0x%08x\ndst=0x%08x\n"
            "stage=%d\ntail_seen=%d\nelapsed_us=%llu\n"
            "checksum_src=0x%08x\nchecksum_dst=0x%08x\n"
            "mismatches=%u\nfirst_mismatch=0x%08x\n",
            M6II_EDMAC_TEST_SIZE, M6II_EDMAC_ROWS, M6II_EDMAC_ROW_BYTES,
            M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH,
            (uint32_t)src, (uint32_t)dst, stage, tail_seen, elapsed_us,
            checksum_src, checksum_dst, mismatches, first_mismatch);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    if (dst) _free_dma_memory((void *)dst);
    if (src) _free_dma_memory((void *)src);

    if (stage == 7 && tail_seen && mismatches == 0)
        NotifyBox(8000, "M6II Esub5 DMA PASS: 1 MiB in %llu us", elapsed_us);
    else
        NotifyBox(8000, "M6II Esub5 DMA FAIL: stage=%d tail=%d mismatches=%u", stage, tail_seen, mismatches);

    m6ii_edmac_busy = 0;
}

static struct menu_entry m6ii_edmac_menu[] = {
    {
        .name   = "TEST M6II Esub5 DMA",
        .priv   = m6ii_edmac_test_run,
        .select = run_in_separate_task,
        .help   = "Experimental one-shot 1 MiB RAM copy via Canon's native M6II DIGIC 8 Esub5 path."
    },
};

static void m6ii_edmac_test_init()
{
    menu_add("Debug", m6ii_edmac_menu, COUNT(m6ii_edmac_menu));
    DryosDebugMsg(0, 15, "M6II native Esub5 DMA test: menu registered");
}

INIT_FUNC(__FILE__, m6ii_edmac_test_init);

#endif /* !CONFIG_HELLO_WORLD */
