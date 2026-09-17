/** \file
 * EOS M6 Mark II DIGIC 8 EDMAC RAM-to-RAM bring-up test.
 *
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
#define M6II_EDMAC_RD_CH            46
#define M6II_EDMAC_WR_CH            13
#define M6II_EDMAC_BOOMER_SELECTOR  0x373f0126
#define M6II_EDMAC_MODE             0
#define M6II_EDMAC_LOG              "M6II_EDMAC_TEST.TXT"

extern void *_alloc_dma_memory(size_t size);
extern void _free_dma_memory(void *ptr);

extern void PwrMng_WakeSubChips(const uint32_t *list);
extern void PwrMng_SuspendSubChips(const uint32_t *list);
extern struct LockEntry *CreateResLockEntry(uint32_t *resources, uint32_t count);
extern unsigned int LockEngineResources(struct LockEntry *lockEntry);
extern unsigned int M6II_UnLockEngineResources(struct LockEntry *lockEntry);

extern void edmac_reset_channel(uint32_t channel);
extern void edmac_reset_boomer_vdkick(uint32_t channel);
extern void edmac_reset_packunpack_mode(uint32_t channel);
extern void edmac_set_address(uint32_t channel, void *addr);
extern void edmac_set_size(uint32_t channel, struct edmac_info *info);
extern void edmac_set_transfer_mode(uint32_t channel, uint32_t mode);
extern void edmac_select_boomer(uint32_t channel, uint32_t selector);
extern void edmac_stop_boomer_maybe(uint32_t channel);
extern void StartEDmac_maybe(uint32_t channel);
extern void ConnectReadEDmac_maybe(uint32_t channel);

static volatile int m6ii_edmac_busy = 0;
static const uint32_t m6ii_edmac_devices[] = {0, 7};
static uint32_t m6ii_edmac_resources[] = {0x100AD, 0x100BB};

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
    struct LockEntry *lock = 0;
    int stage = 0;
    int tail_seen = 0;
    uint32_t mismatches = 0;
    uint32_t first_mismatch = 0xffffffff;
    uint32_t checksum_src = 0;
    uint32_t checksum_dst = 0;
    uint64_t elapsed_us = 0;
    unsigned int lock_ret = 0xffffffff;
    unsigned int unlock_ret = 0xffffffff;

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
    /* Make the last word a guaranteed non-zero completion sentinel. */
    src[words - 1] = 0x4d36444d; /* 'M6DM' */
    dst[words - 1] = 0;
    checksum_src = m6ii_checksum32(src, M6II_EDMAC_TEST_SIZE);
    stage = 1;

    lock = CreateResLockEntry(m6ii_edmac_resources, COUNT(m6ii_edmac_resources));
    if (!lock)
    {
        stage = -2;
        goto cleanup;
    }
    lock_ret = LockEngineResources(lock);
    stage = 2;

    /* D8 EDMAC MMIO is unsafe while its power domain sleeps. */
    PwrMng_WakeSubChips(m6ii_edmac_devices);
    stage = 3;

    edmac_reset_channel(M6II_EDMAC_RD_CH);
    edmac_reset_channel(M6II_EDMAC_WR_CH);
    edmac_reset_boomer_vdkick(M6II_EDMAC_WR_CH);
    edmac_select_boomer(M6II_EDMAC_WR_CH, M6II_EDMAC_BOOMER_SELECTOR);
    edmac_reset_packunpack_mode(M6II_EDMAC_RD_CH);
    edmac_reset_packunpack_mode(M6II_EDMAC_WR_CH);

    struct edmac_info info;
    memset(&info, 0, sizeof(info));
    info.xb = M6II_EDMAC_ROW_BYTES;
    info.yb = M6II_EDMAC_ROWS - 1;
    info.off1b = 0;

    edmac_set_address(M6II_EDMAC_WR_CH, (void *)dst);
    edmac_set_address(M6II_EDMAC_RD_CH, (void *)src);
    edmac_set_size(M6II_EDMAC_WR_CH, &info);
    edmac_set_size(M6II_EDMAC_RD_CH, &info);
    edmac_set_transfer_mode(M6II_EDMAC_WR_CH, M6II_EDMAC_MODE);
    edmac_set_transfer_mode(M6II_EDMAC_RD_CH, M6II_EDMAC_MODE);
    stage = 4;

    uint64_t t0 = get_us_clock();
    StartEDmac_maybe(M6II_EDMAC_WR_CH);
    StartEDmac_maybe(M6II_EDMAC_RD_CH);
    ConnectReadEDmac_maybe(M6II_EDMAC_RD_CH);

    /* Poll only a DMA-memory sentinel; no peripheral reads are performed here. */
    for (int i = 0; i < 100; i++)
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

    edmac_stop_boomer_maybe(M6II_EDMAC_WR_CH);
    PwrMng_SuspendSubChips(m6ii_edmac_devices);
    unlock_ret = M6II_UnLockEngineResources(lock);
    lock = 0;
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
    /* If setup failed after the lock but before normal cleanup, unwind safely. */
    if (lock)
    {
        if (stage >= 3)
            PwrMng_SuspendSubChips(m6ii_edmac_devices);
        unlock_ret = M6II_UnLockEngineResources(lock);
    }

    FILE *f = FIO_CreateFile(M6II_EDMAC_LOG);
    if (f)
    {
        char text[768];
        int len = snprintf(text, sizeof(text),
            "M6II DIGIC8 EDMAC RAM copy v1\n"
            "size=%u\nrows=%u\nrow_bytes=%u\n"
            "rd_channel=%u\nwr_channel=%u\nboomer=0x%08x\n"
            "src=0x%08x\ndst=0x%08x\n"
            "stage=%d\nlock_ret=0x%08x\nunlock_ret=0x%08x\n"
            "tail_seen=%d\nelapsed_us=%llu\n"
            "checksum_src=0x%08x\nchecksum_dst=0x%08x\n"
            "mismatches=%u\nfirst_mismatch=0x%08x\n",
            M6II_EDMAC_TEST_SIZE, M6II_EDMAC_ROWS, M6II_EDMAC_ROW_BYTES,
            M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH, M6II_EDMAC_BOOMER_SELECTOR,
            (uint32_t)src, (uint32_t)dst, stage, lock_ret, unlock_ret,
            tail_seen, elapsed_us, checksum_src, checksum_dst,
            mismatches, first_mismatch);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    if (dst) _free_dma_memory((void *)dst);
    if (src) _free_dma_memory((void *)src);

    if (stage == 7 && tail_seen && mismatches == 0)
        NotifyBox(8000, "D8 EDMAC COPY PASS: 1 MiB in %llu us", elapsed_us);
    else
        NotifyBox(8000, "D8 EDMAC TEST FAIL: stage=%d tail=%d mismatches=%u", stage, tail_seen, mismatches);

    m6ii_edmac_busy = 0;
}

static struct menu_entry m6ii_edmac_menu[] = {
    {
        .name   = "TEST D8 EDMAC memcpy",
        .priv   = m6ii_edmac_test_run,
        .select = run_in_separate_task,
        .help   = "Experimental: copy 1 MiB between DMA buffers using DIGIC 8 EDMAC channels 46/13."
    },
};

static void m6ii_edmac_test_init()
{
    menu_add("Debug", m6ii_edmac_menu, COUNT(m6ii_edmac_menu));
    DryosDebugMsg(0, 15, "M6II D8 EDMAC test: menu registered");
}

INIT_FUNC(__FILE__, m6ii_edmac_test_init);

#endif /* !CONFIG_HELLO_WORLD */
