/** \file
 * EOS M6 Mark II DIGIC 8 EDMAC RAM-to-RAM bring-up test.
 *
 * Uses Canon's own M6II MemoryToMemoryEsub5 wrappers and configuration
 * recovered from ROM0. This deliberately copies a known 1 MiB pattern
 * between two DMA buffers. It does not touch the live RAW writer channel.
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

/* Exact M6II MemoryToMemoryEsub5 configuration from ROM table 0xE11750DC. */
#define M6II_EDMAC_RD_CH            61      /* 0x3D */
#define M6II_EDMAC_WR_CH            24      /* 0x18 */
#define M6II_EDMAC_MODE             1
#define M6II_EDMAC_BOOMER           0x00C2060D
#define M6II_EDMAC_LOG              "M6II_EDMAC_TEST.TXT"

typedef void (*m6ii_esub5_setup_fn)(const uint32_t *channels);
typedef void (*m6ii_esub5_callback_fn)(void (*cbr)(void *), void *ctx);
typedef void (*m6ii_esub5_void_fn)(void);
typedef void (*m6ii_esub5_addr_fn)(const uint32_t *addresses);
typedef void (*m6ii_esub5_size_fn)(const uintptr_t *args);
typedef void (*m6ii_power_fn)(const uint32_t *list);
typedef struct LockEntry *(*m6ii_create_lock_fn)(uint32_t *resources, uint32_t count);
typedef unsigned int (*m6ii_lock_fn)(struct LockEntry *lock);

/* ROM0 is based at 0xE0000000. These are the actual M6II 1.1.1 addresses. */
#define M6II_ESUB5_SETUP       ((m6ii_esub5_setup_fn)(0xE0639898u | 1u))
#define M6II_ESUB5_CLEANUP     ((m6ii_esub5_void_fn)(0xE0639922u | 1u))
#define M6II_ESUB5_SET_CBR     ((m6ii_esub5_callback_fn)(0xE063993Cu | 1u))
#define M6II_ESUB5_RESET_CBR   ((m6ii_esub5_void_fn)(0xE0639946u | 1u))
#define M6II_ESUB5_START       ((m6ii_esub5_void_fn)(0xE0639952u | 1u))
#define M6II_ESUB5_SET_ADDR    ((m6ii_esub5_addr_fn)(0xE063996Cu | 1u))
#define M6II_ESUB5_SET_SIZE    ((m6ii_esub5_size_fn)(0xE0639986u | 1u))

#define M6II_PWR_WAKE          ((m6ii_power_fn)(0xE0630EB6u | 1u))
#define M6II_PWR_SUSPEND       ((m6ii_power_fn)(0xE0630EE4u | 1u))
#define M6II_CREATE_LOCK       ((m6ii_create_lock_fn)(0xE057978Au | 1u))
#define M6II_LOCK_RESOURCES    ((m6ii_lock_fn)(0xE0579972u | 1u))
#define M6II_UNLOCK_RESOURCES  ((m6ii_lock_fn)(0xE0579A0Cu | 1u))
#define M6II_DELETE_LOCK       ((m6ii_lock_fn)(0xE057986Eu | 1u))

extern void *_alloc_dma_memory(size_t size);
extern void _free_dma_memory(void *ptr);

static volatile int m6ii_edmac_busy = 0;
static volatile int m6ii_esub5_done = 0;

/* Canon path e0617ab8 -> e0637798/e063785c. */
static uint32_t m6ii_esub5_resources[] = { 0x0005001F, 0x00050023 };
static const uint32_t m6ii_esub5_devices[] = { 4, 7 };

static void m6ii_esub5_done_cbr(void *ctx)
{
    (void)ctx;
    m6ii_esub5_done = 1;
}

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

    m6ii_edmac_busy = 1;
    volatile uint32_t *src = 0;
    volatile uint32_t *dst = 0;
    struct LockEntry *lock = 0;
    int stage = 0;
    int tail_seen = 0;
    int domain_awake = 0;
    int esub_setup = 0;
    int callback_set = 0;
    uint32_t mismatches = 0;
    uint32_t first_mismatch = 0xffffffff;
    uint32_t checksum_src = 0;
    uint32_t checksum_dst = 0;
    uint64_t elapsed_us = 0;
    unsigned int lock_ret = 0xffffffff;
    unsigned int unlock_ret = 0xffffffff;
    unsigned int delete_ret = 0xffffffff;

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
    src[words - 1] = 0x4d36444d;
    dst[words - 1] = 0;
    checksum_src = m6ii_checksum32(src, M6II_EDMAC_TEST_SIZE);
    stage = 1;

    /* Same resource lock used by Canon's caller of MemoryToMemoryEsub5. */
    lock = M6II_CREATE_LOCK(m6ii_esub5_resources, COUNT(m6ii_esub5_resources));
    if (!lock)
    {
        stage = -2;
        goto cleanup;
    }
    lock_ret = M6II_LOCK_RESOURCES(lock);
    stage = 2;

    /* Canon's matching power-domain list is {4,7}. */
    M6II_PWR_WAKE(m6ii_esub5_devices);
    domain_awake = 1;
    stage = 3;

    const uint32_t channels[2] = { M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH };
    M6II_ESUB5_SETUP(channels);
    esub_setup = 1;

    m6ii_esub5_done = 0;
    M6II_ESUB5_SET_CBR(m6ii_esub5_done_cbr, 0);
    callback_set = 1;
    stage = 4;

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
    stage = 5;

    uint64_t t0 = get_us_clock();
    M6II_ESUB5_START();

    /* Wait only on RAM/callback state, never by reading EDMAC MMIO. */
    for (int i = 0; i < 200; i++)
    {
        if (dst[words - 1] == src[words - 1])
        {
            tail_seen = 1;
            break;
        }
        if (m6ii_esub5_done && dst[words - 1] == src[words - 1])
        {
            tail_seen = 1;
            break;
        }
        msleep(1);
    }
    elapsed_us = get_us_clock() - t0;
    stage = 6;

    M6II_ESUB5_CLEANUP();
    esub_setup = 0;
    M6II_ESUB5_RESET_CBR();
    callback_set = 0;

    M6II_PWR_SUSPEND(m6ii_esub5_devices);
    domain_awake = 0;

    unlock_ret = M6II_UNLOCK_RESOURCES(lock);
    delete_ret = M6II_DELETE_LOCK(lock);
    lock = 0;
    stage = 7;

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
    stage = 8;

cleanup:
    if (esub_setup)
        M6II_ESUB5_CLEANUP();
    if (callback_set)
        M6II_ESUB5_RESET_CBR();
    if (domain_awake)
        M6II_PWR_SUSPEND(m6ii_esub5_devices);
    if (lock)
    {
        unlock_ret = M6II_UNLOCK_RESOURCES(lock);
        delete_ret = M6II_DELETE_LOCK(lock);
    }

    FILE *f = FIO_CreateFile(M6II_EDMAC_LOG);
    if (f)
    {
        char text[1024];
        int len = snprintf(text, sizeof(text),
            "M6II DIGIC8 native Esub5 EDMAC copy v3\n"
            "rom0_base=0xE0000000\n"
            "size=%u\nrows=%u\nrow_bytes=%u\n"
            "rd_channel=%u\nwr_channel=%u\nmode=%u\nboomer=0x%08x\n"
            "resource0=0x%08x\nresource1=0x%08x\npower_subchip=4\n"
            "src=0x%08x\ndst=0x%08x\n"
            "stage=%d\nlock_ret=0x%08x\nunlock_ret=0x%08x\ndelete_ret=0x%08x\n"
            "callback_done=%d\ntail_seen=%d\nelapsed_us=%llu\n"
            "checksum_src=0x%08x\nchecksum_dst=0x%08x\n"
            "mismatches=%u\nfirst_mismatch=0x%08x\n",
            M6II_EDMAC_TEST_SIZE, M6II_EDMAC_ROWS, M6II_EDMAC_ROW_BYTES,
            M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH, M6II_EDMAC_MODE, M6II_EDMAC_BOOMER,
            m6ii_esub5_resources[0], m6ii_esub5_resources[1],
            (uint32_t)src, (uint32_t)dst, stage, lock_ret, unlock_ret, delete_ret,
            m6ii_esub5_done, tail_seen, elapsed_us,
            checksum_src, checksum_dst, mismatches, first_mismatch);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    if (dst) _free_dma_memory((void *)dst);
    if (src) _free_dma_memory((void *)src);

    if (stage == 8 && tail_seen && mismatches == 0)
        NotifyBox(8000, "M6II native DMA PASS: 1 MiB in %llu us", elapsed_us);
    else
        NotifyBox(8000, "M6II native DMA FAIL: stage=%d tail=%d mismatches=%u", stage, tail_seen, mismatches);

    m6ii_edmac_busy = 0;
}

static struct menu_entry m6ii_edmac_menu[] = {
    {
        .name   = "TEST M6II native DMA v3",
        .priv   = m6ii_edmac_test_run,
        .select = run_in_separate_task,
        .help   = "One-shot 1 MiB RAM copy using Canon M6II ROM-derived MemoryToMemoryEsub5 configuration."
    },
};

static void m6ii_edmac_test_init()
{
    menu_add("Debug", m6ii_edmac_menu, COUNT(m6ii_edmac_menu));
    DryosDebugMsg(0, 15, "M6II native Esub5 DMA v3: menu registered");
}

INIT_FUNC(__FILE__, m6ii_edmac_test_init);

#endif /* !CONFIG_HELLO_WORLD */
