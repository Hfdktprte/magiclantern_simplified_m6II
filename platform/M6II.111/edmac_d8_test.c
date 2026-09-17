/** \file
 * EOS M6 Mark II full RAW-frame DMA test.
 *
 * Uses Canon's own M6II MemoryToMemoryEsub5 wrappers and ROM-derived
 * configuration to copy one complete 14-bit LiveView RAW frame into a
 * separate DMA buffer. The RAW source pointer and geometry come from the
 * already-proven M6II LiveView state structure.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <edmac.h>
#include <timer.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE          0x00010970
#define M6II_RAW_WIDTH_EXPECTED      3568
#define M6II_RAW_HEIGHT_EXPECTED     2000
#define M6II_RAW_ROW_BYTES_EXPECTED  ((M6II_RAW_WIDTH_EXPECTED * 7) / 4)
#define M6II_RAW_FRAME_BYTES_EXPECTED (M6II_RAW_ROW_BYTES_EXPECTED * M6II_RAW_HEIGHT_EXPECTED)

/* Exact M6II MemoryToMemoryEsub5 configuration recovered from ROM0. */
#define M6II_EDMAC_RD_CH              61      /* 0x3D */
#define M6II_EDMAC_WR_CH              24      /* 0x18 */
#define M6II_EDMAC_MODE               1
#define M6II_EDMAC_BOOMER             0x00C2060D
#define M6II_EDMAC_LOG                "M6II_RAW_DMA.TXT"

typedef void (*m6ii_esub5_setup_fn)(const uint32_t *channels);
typedef void (*m6ii_esub5_callback_fn)(void (*cbr)(void *), void *ctx);
typedef void (*m6ii_esub5_void_fn)(void);
typedef void (*m6ii_esub5_addr_fn)(const uint32_t *addresses);
typedef void (*m6ii_esub5_size_fn)(const uintptr_t *args);
typedef void (*m6ii_power_fn)(const uint32_t *list);
typedef struct LockEntry *(*m6ii_create_lock_fn)(uint32_t *resources, uint32_t count);
typedef unsigned int (*m6ii_lock_fn)(struct LockEntry *lock);

/* ROM0 is based at 0xE0000000. */
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
static uint32_t m6ii_esub5_resources[] = { 0x0005001F, 0x00050023 };
static const uint32_t m6ii_esub5_devices[] = { 4, 7 };

static uint32_t m6ii_raw_state_read(uint32_t byte_offset)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + byte_offset);
}

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

static void m6ii_raw_dma_test_run()
{
    if (m6ii_edmac_busy)
    {
        NotifyBox(2000, "RAW DMA test already running");
        return;
    }
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW DMA requires active LiveView");
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
    int raw_enabled = 0;
    int domain_awake = 0;
    int esub_setup = 0;
    int callback_set = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_bytes = 0;
    uint32_t frame_bytes = 0;
    uint32_t mismatches = 0;
    uint32_t first_mismatch = 0xffffffff;
    uint32_t checksum_src = 0;
    uint32_t checksum_dst = 0;
    uint32_t elapsed_us = 0xffffffff;
    uint32_t wait_ms = 0;
    unsigned int lock_ret = 0xffffffff;
    unsigned int unlock_ret = 0xffffffff;
    unsigned int delete_ret = 0xffffffff;
    int ret_mm = 0;
    int ret_on = 0;
    int ret_off = 0;

    /* Allocate destination before enabling RAW. */
    dst = (volatile uint32_t *)_alloc_dma_memory(M6II_RAW_FRAME_BYTES_EXPECTED);
    if (!dst)
    {
        stage = -1;
        goto cleanup;
    }
    memset((void *)dst, 0xA5, M6II_RAW_FRAME_BYTES_EXPECTED);
    stage = 1;

    ret_mm = call("lv_set_mm", 1);
    ret_on = call("lv_save_raw", 1);
    raw_enabled = 1;
    msleep(250);

    width = m6ii_raw_state_read(0x10);
    height = m6ii_raw_state_read(0x14);
    src = (volatile uint32_t *)m6ii_raw_state_read(0x58);
    if (!src || width != M6II_RAW_WIDTH_EXPECTED || height != M6II_RAW_HEIGHT_EXPECTED)
    {
        stage = -2;
        goto cleanup;
    }

    row_bytes = (width * 7) / 4;
    frame_bytes = row_bytes * height;
    if (row_bytes != M6II_RAW_ROW_BYTES_EXPECTED || frame_bytes != M6II_RAW_FRAME_BYTES_EXPECTED)
    {
        stage = -3;
        goto cleanup;
    }
    stage = 2;

    lock = M6II_CREATE_LOCK(m6ii_esub5_resources, COUNT(m6ii_esub5_resources));
    if (!lock)
    {
        stage = -4;
        goto cleanup;
    }
    lock_ret = M6II_LOCK_RESOURCES(lock);
    stage = 3;

    M6II_PWR_WAKE(m6ii_esub5_devices);
    domain_awake = 1;

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
    src_info.xb = row_bytes;
    src_info.yb = height - 1;
    dst_info.xb = row_bytes;
    dst_info.yb = height - 1;

    const uint32_t addresses[2] = { (uint32_t)src, (uint32_t)dst };
    const uintptr_t size_args[3] = {
        (uintptr_t)&src_info,
        (uintptr_t)&dst_info,
        M6II_EDMAC_MODE
    };
    M6II_ESUB5_SET_ADDR(addresses);
    M6II_ESUB5_SET_SIZE(size_args);
    stage = 5;

    uint32_t t0 = (uint32_t)get_us_clock();
    M6II_ESUB5_START();

    /* Completion comes from Canon's own EDMAC callback path. */
    for (wait_ms = 0; wait_ms < 500; wait_ms++)
    {
        if (m6ii_esub5_done)
            break;
        msleep(1);
    }
    elapsed_us = (uint32_t)get_us_clock() - t0;
    stage = 6;

    /* Freeze the Canon RAW source before verification. */
    ret_off = call("lv_save_raw", 0);
    raw_enabled = 0;

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

    checksum_src = m6ii_checksum32(src, frame_bytes);
    checksum_dst = m6ii_checksum32(dst, frame_bytes);
    for (uint32_t i = 0; i < frame_bytes / 4; i++)
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
    if (raw_enabled)
    {
        ret_off = call("lv_save_raw", 0);
        raw_enabled = 0;
    }
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
        char text[1280];
        int len = snprintf(text, sizeof(text),
            "M6II RAW full-frame native DMA v4\n"
            "rom0_base=0xE0000000\n"
            "expected_width=0x%08x\nexpected_height=0x%08x\n"
            "width=0x%08x\nheight=0x%08x\nrow_bytes=0x%08x\nframe_bytes=0x%08x\n"
            "rd_channel=0x%08x\nwr_channel=0x%08x\nmode=0x%08x\nboomer=0x%08x\n"
            "resource0=0x%08x\nresource1=0x%08x\npower_subchip=0x00000004\n"
            "raw_src=0x%08x\ndma_dst=0x%08x\n"
            "stage=0x%08x\nret_mm=0x%08x\nret_raw_on=0x%08x\nret_raw_off=0x%08x\n"
            "lock_ret=0x%08x\nunlock_ret=0x%08x\ndelete_ret=0x%08x\n"
            "callback_done=0x%08x\nwait_ms=0x%08x\nelapsed_us=0x%08x\n"
            "checksum_src=0x%08x\nchecksum_dst=0x%08x\n"
            "mismatches=0x%08x\nfirst_mismatch=0x%08x\n",
            M6II_RAW_WIDTH_EXPECTED, M6II_RAW_HEIGHT_EXPECTED,
            width, height, row_bytes, frame_bytes,
            M6II_EDMAC_RD_CH, M6II_EDMAC_WR_CH, M6II_EDMAC_MODE, M6II_EDMAC_BOOMER,
            m6ii_esub5_resources[0], m6ii_esub5_resources[1],
            (uint32_t)src, (uint32_t)dst,
            (uint32_t)stage, (uint32_t)ret_mm, (uint32_t)ret_on, (uint32_t)ret_off,
            lock_ret, unlock_ret, delete_ret,
            (uint32_t)m6ii_esub5_done, wait_ms, elapsed_us,
            checksum_src, checksum_dst, mismatches, first_mismatch);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    if (dst)
        _free_dma_memory((void *)dst);

    if (stage == 8 && m6ii_esub5_done && mismatches == 0)
        NotifyBox(8000, "RAW DMA PASS: elapsed=0x%08x us", elapsed_us);
    else
        NotifyBox(8000, "RAW DMA result: stage=0x%08x done=%x mism=0x%08x", (uint32_t)stage, m6ii_esub5_done, mismatches);

    m6ii_edmac_busy = 0;
}

static struct menu_entry m6ii_edmac_menu[] = {
    {
        .name   = "TEST full RAW frame DMA v4",
        .priv   = m6ii_raw_dma_test_run,
        .select = run_in_separate_task,
        .help   = "Copies one 3568x2000 14-bit Canon RAW LiveView frame via the ROM-derived M6II Esub5 DMA path."
    },
};

static void m6ii_edmac_test_init()
{
    menu_add("Debug", m6ii_edmac_menu, COUNT(m6ii_edmac_menu));
    DryosDebugMsg(0, 15, "M6II full RAW DMA v4: menu registered");
}

INIT_FUNC(__FILE__, m6ii_edmac_test_init);

#endif /* !CONFIG_HELLO_WORLD */
