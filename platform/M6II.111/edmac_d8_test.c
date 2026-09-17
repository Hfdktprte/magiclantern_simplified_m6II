/** \file
 * EOS M6 Mark II ROM-derived FrameToLinear RAW tap test.
 *
 * v4 proved that feeding the CPU-visible RAW pointer directly to generic
 * MemoryToMemoryEsub5 can hard-lock. Canon's own image path instead maps a
 * 64-KiB-aligned frame-memory base through MemifWrap and uses CopyEsub6,
 * channels 0x40 -> 0x1A. This test reproduces that path for ONE line only.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <timer.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE       0x00010970u
#define M6II_RAW_WIDTH_EXPECTED   3568u
#define M6II_RAW_HEIGHT_EXPECTED  2000u
#define M6II_TAP_DST_BYTES        (1024u * 1024u)
#define M6II_TAP_FILL             0xA5u
#define M6II_TAP_LOG              "M6II_FRAME_TAP.TXT"

/* Canon FrameToLinear fixed configuration from ROM table 0xE0CF49C0. */
#define M6II_FRAME_RD_CH          0x40u
#define M6II_LINEAR_WR_CH         0x1Au
static uint32_t m6ii_frame_resources[] = { 0x00060064u, 0x0006006Eu };
static const uint32_t m6ii_frame_devices[] = { 5u, 7u };
static const uint32_t m6ii_frame_channels[] = { M6II_FRAME_RD_CH, M6II_LINEAR_WR_CH };

typedef void (*m6ii_void_fn)(void);
typedef void (*m6ii_ptr_fn)(const void *p);
typedef uint32_t (*m6ii_memif_fn)(uint32_t index, uint32_t base, uint32_t bandwidth_bytes);
typedef void (*m6ii_power_fn)(const uint32_t *list);
typedef struct LockEntry *(*m6ii_create_lock_fn)(uint32_t *resources, uint32_t count);
typedef unsigned int (*m6ii_lock_fn)(struct LockEntry *lock);

/* Canon's FrameLinearCopyMultiShot / CopyEsub6 wrappers. */
#define M6II_COPYE6_INIT       ((m6ii_ptr_fn)(0xE009DC54u | 1u))
#define M6II_COPYE6_TRANSFER   ((m6ii_ptr_fn)(0xE009DC80u | 1u))
#define M6II_COPYE6_CLEANUP    ((m6ii_void_fn)(0xE009DCF0u | 1u))

/* Frame-memory resolver used by Canon FrameToLinear. */
#define M6II_MEMIF_WRAP        ((m6ii_memif_fn)(0xE0065624u | 1u))

#define M6II_PWR_WAKE          ((m6ii_power_fn)(0xE0630EB6u | 1u))
#define M6II_PWR_SUSPEND       ((m6ii_power_fn)(0xE0630EE4u | 1u))
#define M6II_CREATE_LOCK       ((m6ii_create_lock_fn)(0xE057978Au | 1u))
#define M6II_LOCK_RESOURCES    ((m6ii_lock_fn)(0xE0579972u | 1u))
#define M6II_UNLOCK_RESOURCES  ((m6ii_lock_fn)(0xE0579A0Cu | 1u))
#define M6II_DELETE_LOCK       ((m6ii_lock_fn)(0xE057986Eu | 1u))

extern void *_alloc_dma_memory(size_t size);
extern void _free_dma_memory(void *ptr);

static volatile int m6ii_frame_tap_busy = 0;

static uint32_t m6ii_raw_state_read(uint32_t byte_offset)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + byte_offset);
}

static uint32_t m6ii_round_up_512(uint32_t v)
{
    return (v + 0x1ffu) & ~0x1ffu;
}

static uint32_t m6ii_changed_bytes(const volatile uint8_t *p, uint32_t n)
{
    uint32_t changed = 0;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != M6II_TAP_FILL)
            changed++;
    return changed;
}

static uint32_t m6ii_checksum32(const volatile uint32_t *p, uint32_t bytes)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < bytes / 4; i++)
        sum = (sum << 5) - sum + p[i];
    return sum;
}

static void m6ii_frame_tap_run()
{
    if (m6ii_frame_tap_busy)
    {
        NotifyBox(2000, "Frame tap already running");
        return;
    }
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "Frame tap requires active LiveView");
        return;
    }
    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_frame_tap_busy = 1;

    volatile uint8_t *dst = 0;
    struct LockEntry *lock = 0;
    int raw_enabled = 0;
    int domain_awake = 0;
    int copye6_ready = 0;
    int stage = 0;
    int ret_mm = 0, ret_on = 0, ret_off = 0;
    unsigned int lock_ret = 0xffffffffu;
    unsigned int unlock_ret = 0xffffffffu;
    unsigned int delete_ret = 0xffffffffu;

    uint32_t width = 0, height = 0, raw_ptr = 0;
    uint32_t map_base = 0, map_offset = 0;
    uint32_t bandwidth = 0;
    uint32_t offset_units = 0, src_x = 0, src_y = 0;
    uint32_t memif_handle = 0;
    uint32_t elapsed_us = 0xffffffffu;
    uint32_t changed_first_64k = 0;
    uint32_t checksum_first_64k = 0;
    uint32_t first_words[16];
    memset(first_words, 0, sizeof(first_words));

    dst = (volatile uint8_t *)_alloc_dma_memory(M6II_TAP_DST_BYTES);
    if (!dst)
    {
        stage = -1;
        goto cleanup;
    }
    memset((void *)dst, M6II_TAP_FILL, M6II_TAP_DST_BYTES);
    stage = 1;

    ret_mm = call("lv_set_mm", 1);
    ret_on = call("lv_save_raw", 1);
    raw_enabled = 1;
    msleep(250);

    width = m6ii_raw_state_read(0x10);
    height = m6ii_raw_state_read(0x14);
    raw_ptr = m6ii_raw_state_read(0x58);
    if (!raw_ptr || width != M6II_RAW_WIDTH_EXPECTED || height != M6II_RAW_HEIGHT_EXPECTED)
    {
        stage = -2;
        goto cleanup;
    }

    /*
     * Canon FrameToLinear does not pass this CPU pointer to EDMAC.
     * It registers a 64-KiB-aligned frame-memory base with MemifWrap.
     * CopyEsub6 then addresses within that mapped frame using X/Y units.
     */
    map_base = raw_ptr & 0xffff0000u;
    map_offset = raw_ptr - map_base;
    bandwidth = m6ii_round_up_512(width);   /* Canon rounds BW to 512. */

    /* Canon FrameToLinear type 1 uses 16 bits/unit on the frame side. */
    if ((map_offset & 1u) || !bandwidth)
    {
        stage = -3;
        goto cleanup;
    }
    offset_units = map_offset / 2u;
    src_y = offset_units / bandwidth;
    src_x = offset_units % bandwidth;

    /* MemifWrap(index=0, 64-KiB aligned base, 2*BW bytes). */
    memif_handle = M6II_MEMIF_WRAP(0, map_base, bandwidth * 2u);
    if (!memif_handle)
    {
        stage = -4;
        goto cleanup;
    }
    stage = 2;

    lock = M6II_CREATE_LOCK(m6ii_frame_resources, COUNT(m6ii_frame_resources));
    if (!lock)
    {
        stage = -5;
        goto cleanup;
    }
    lock_ret = M6II_LOCK_RESOURCES(lock);
    stage = 3;

    M6II_PWR_WAKE(m6ii_frame_devices);
    domain_awake = 1;

    M6II_COPYE6_INIT(m6ii_frame_channels);
    copye6_ready = 1;
    stage = 4;

    /*
     * Exact 13-word descriptor shape produced by Canon FrameToLinear():
     * source type/base/BW/X/Y, destination type/base/BW/X/Y,
     * copy width/lines, Memif handle.
     *
     * We request ONE line only. At type 1, one full 3584-unit line is
     * roughly 7 KiB, while the destination allocation is 1 MiB.
     */
    uint32_t d[13];
    memset(d, 0, sizeof(d));
    d[0]  = 1u;                       /* source format */
    d[1]  = map_base;                 /* used by Memif mapping; CopyEsub6 zeroes direct src base */
    d[2]  = bandwidth;
    d[3]  = src_x;
    d[4]  = src_y;
    d[5]  = 1u;                       /* destination format */
    d[6]  = (uint32_t)dst;
    d[7]  = bandwidth;
    d[8]  = 0u;
    d[9]  = 0u;
    d[10] = bandwidth;
    d[11] = 1u;                       /* one line */
    d[12] = memif_handle;

    uint32_t t0 = (uint32_t)get_us_clock();
    M6II_COPYE6_TRANSFER(d);           /* Canon wrapper includes completion wait/timeout. */
    elapsed_us = (uint32_t)get_us_clock() - t0;
    stage = 5;

    M6II_COPYE6_CLEANUP();
    copye6_ready = 0;
    M6II_PWR_SUSPEND(m6ii_frame_devices);
    domain_awake = 0;
    unlock_ret = M6II_UNLOCK_RESOURCES(lock);
    delete_ret = M6II_DELETE_LOCK(lock);
    lock = 0;

    ret_off = call("lv_save_raw", 0);
    raw_enabled = 0;
    stage = 6;

    changed_first_64k = m6ii_changed_bytes(dst, 65536u);
    checksum_first_64k = m6ii_checksum32((const volatile uint32_t *)dst, 65536u);
    for (int i = 0; i < 16; i++)
        first_words[i] = ((volatile uint32_t *)dst)[i];
    stage = 7;

cleanup:
    if (copye6_ready)
        M6II_COPYE6_CLEANUP();
    if (domain_awake)
        M6II_PWR_SUSPEND(m6ii_frame_devices);
    if (lock)
    {
        unlock_ret = M6II_UNLOCK_RESOURCES(lock);
        delete_ret = M6II_DELETE_LOCK(lock);
    }
    if (raw_enabled)
    {
        ret_off = call("lv_save_raw", 0);
        raw_enabled = 0;
    }

    FILE *f = FIO_CreateFile(M6II_TAP_LOG);
    if (f)
    {
        char text[1800];
        int len = snprintf(text, sizeof(text),
            "M6II FrameToLinear RAW tap v5\n"
            "width=0x%08x\nheight=0x%08x\nraw_ptr=0x%08x\n"
            "map_base=0x%08x\nmap_offset=0x%08x\nbandwidth=0x%08x\n"
            "src_x=0x%08x\nsrc_y=0x%08x\nmemif_handle=0x%08x\n"
            "frame_rd_ch=0x%08x\nlinear_wr_ch=0x%08x\n"
            "resource0=0x%08x\nresource1=0x%08x\npower_subchip=0x00000005\n"
            "dst=0x%08x\nstage=0x%08x\n"
            "ret_mm=0x%08x\nret_raw_on=0x%08x\nret_raw_off=0x%08x\n"
            "lock_ret=0x%08x\nunlock_ret=0x%08x\ndelete_ret=0x%08x\n"
            "elapsed_us=0x%08x\nchanged_first_64k=0x%08x\nchecksum_first_64k=0x%08x\n"
            "w00=0x%08x w01=0x%08x w02=0x%08x w03=0x%08x\n"
            "w04=0x%08x w05=0x%08x w06=0x%08x w07=0x%08x\n"
            "w08=0x%08x w09=0x%08x w10=0x%08x w11=0x%08x\n"
            "w12=0x%08x w13=0x%08x w14=0x%08x w15=0x%08x\n",
            width, height, raw_ptr,
            map_base, map_offset, bandwidth,
            src_x, src_y, memif_handle,
            M6II_FRAME_RD_CH, M6II_LINEAR_WR_CH,
            m6ii_frame_resources[0], m6ii_frame_resources[1],
            (uint32_t)dst, (uint32_t)stage,
            (uint32_t)ret_mm, (uint32_t)ret_on, (uint32_t)ret_off,
            lock_ret, unlock_ret, delete_ret,
            elapsed_us, changed_first_64k, checksum_first_64k,
            first_words[0], first_words[1], first_words[2], first_words[3],
            first_words[4], first_words[5], first_words[6], first_words[7],
            first_words[8], first_words[9], first_words[10], first_words[11],
            first_words[12], first_words[13], first_words[14], first_words[15]);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    if (dst)
        _free_dma_memory((void *)dst);

    if (stage == 7 && changed_first_64k != 0)
        NotifyBox(8000, "FRAME TAP CHANGED RAM: 0x%08x bytes", changed_first_64k);
    else
        NotifyBox(8000, "FRAME TAP result stage=0x%08x changed=0x%08x", (uint32_t)stage, changed_first_64k);

    m6ii_frame_tap_busy = 0;
}

static struct menu_entry m6ii_frame_tap_menu[] = {
    {
        .name   = "TEST RAW frame tap v5",
        .priv   = m6ii_frame_tap_run,
        .select = run_in_separate_task,
        .help   = "One-line Canon FrameToLinear/CopyEsub6 tap using M6II ROM-derived channels/resources."
    },
};

static void m6ii_frame_tap_init()
{
    menu_add("Debug", m6ii_frame_tap_menu, COUNT(m6ii_frame_tap_menu));
    DryosDebugMsg(0, 15, "M6II FrameToLinear RAW tap v5: menu registered");
}

INIT_FUNC(__FILE__, m6ii_frame_tap_init);

#endif /* !CONFIG_HELLO_WORLD */
