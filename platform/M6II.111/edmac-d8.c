/** \file
 * EOS M6 Mark II DIGIC 8 EDMAC memcpy backend for mlv_lite.
 *
 * Uses Canon's native MemoryToMemoryEsub5 path recovered from M6II 1.1.1 ROM.
 * Keep the generic mlv_lite recorder unchanged: each rectangle copy is fully
 * completed and the native Esub5 path is torn down before this backend reports
 * write completion to the caller.
 */

#include "edmac-memcpy.h"
#include "dryos.h"
#include "edmac.h"
#include "arm-mcr.h"

#define M6II_MEM2MEM_RD_CH 61u
#define M6II_MEM2MEM_WR_CH 24u
#define M6II_MEM2MEM_MODE  1u
#define M6II_MEM2MEM_WAIT_MS 200u

static uint32_t m6ii_mem2mem_resources[] = { 0x0005001Fu, 0x00050023u };
static const uint32_t m6ii_mem2mem_devices[] = { 4u, 7u };

/* Public symbols expected by mlv_lite. */
uint32_t edmac_read_chan  = M6II_MEM2MEM_RD_CH;
uint32_t edmac_write_chan = M6II_MEM2MEM_WR_CH;

struct LockEntry;
typedef void (*m6ii_esub5_setup_fn)(const uint32_t *channels);
typedef void (*m6ii_esub5_callback_fn)(void (*cbr)(void *), void *ctx);
typedef void (*m6ii_esub5_void_fn)(void);
typedef void (*m6ii_esub5_addr_fn)(const uint32_t *addresses);
typedef void (*m6ii_esub5_size_fn)(const uintptr_t *args);
typedef struct LockEntry *(*m6ii_create_lock_fn)(uint32_t *resources, uint32_t count);
typedef unsigned int (*m6ii_lock_fn)(struct LockEntry *lock);
typedef void (*m6ii_power_fn)(const uint32_t *list);

/* M6II 1.1.1 ROM0 base = 0xE0000000. */
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

static struct LockEntry *m6ii_mem2mem_lock = 0;
static volatile int m6ii_copy_done = 0;

static void m6ii_copy_done_cbr(void *ctx)
{
    (void)ctx;
    m6ii_copy_done = 1;
}

void edmac_memcpy_res_lock(void)
{
    if (m6ii_mem2mem_lock)
        return;

    m6ii_mem2mem_lock = M6II_CREATE_LOCK(
        m6ii_mem2mem_resources,
        COUNT(m6ii_mem2mem_resources)
    );
    if (!m6ii_mem2mem_lock)
        return;

    M6II_LOCK_RESOURCES(m6ii_mem2mem_lock);
    M6II_PWR_WAKE(m6ii_mem2mem_devices);
}

void edmac_memcpy_res_unlock(void)
{
    if (!m6ii_mem2mem_lock)
        return;

    M6II_PWR_SUSPEND(m6ii_mem2mem_devices);
    M6II_UNLOCK_RESOURCES(m6ii_mem2mem_lock);
    M6II_DELETE_LOCK(m6ii_mem2mem_lock);
    m6ii_mem2mem_lock = 0;
}

void* edmac_copy_rectangle_cbr_start(void *dst, void *src,
                                     int src_width, int src_x, int src_y,
                                     int dst_width, int dst_x, int dst_y,
                                     int w, int h,
                                     void (*cbr_r)(void *), void (*cbr_w)(void *),
                                     void *cbr_ctx)
{
    if (!src || !dst || w <= 0 || h <= 0)
        return 0;

    /*
     * Match the working DIGIC 8 backend contract: this function may be named
     * "..._start", but for D8 it completes the native memory-to-memory path
     * before reporting completion to mlv_lite.
     */
    if (src != UNCACHEABLE(src))
        sync_caches();

    uint32_t src_adjusted = (uint32_t)src + src_x + src_y * src_width;
    uint32_t dst_adjusted = (uint32_t)dst + dst_x + dst_y * dst_width;

    struct edmac_info src_region;
    struct edmac_info dst_region;
    memset(&src_region, 0, sizeof(src_region));
    memset(&dst_region, 0, sizeof(dst_region));

    src_region.off1b = src_width - w;
    src_region.xb = w;
    src_region.yb = h - 1;

    dst_region.off1b = dst_width - w;
    dst_region.xb = w;
    dst_region.yb = h - 1;

    const uint32_t channels[2] = {
        M6II_MEM2MEM_RD_CH,
        M6II_MEM2MEM_WR_CH
    };
    const uint32_t addresses[2] = {
        src_adjusted,
        dst_adjusted
    };
    const uintptr_t size_args[3] = {
        (uintptr_t)&src_region,
        (uintptr_t)&dst_region,
        M6II_MEM2MEM_MODE
    };

    /*
     * Same lifecycle as the proven M6II native DMA v3 test:
     * setup -> callback -> address/size -> start -> completion
     * -> cleanup -> reset callback.
     */
    m6ii_copy_done = 0;
    M6II_ESUB5_SETUP(channels);
    M6II_ESUB5_SET_CBR(m6ii_copy_done_cbr, 0);
    M6II_ESUB5_SET_ADDR(addresses);
    M6II_ESUB5_SET_SIZE(size_args);
    M6II_ESUB5_START();

    uint32_t waited = 0;
    while (!m6ii_copy_done && waited < M6II_MEM2MEM_WAIT_MS)
    {
        msleep(1);
        waited++;
    }

    M6II_ESUB5_CLEANUP();
    M6II_ESUB5_RESET_CBR();

    if (!m6ii_copy_done)
    {
        /* Do not report completion to mlv_lite on a failed native transfer. */
        return 0;
    }

    if (cbr_r)
        cbr_r(cbr_ctx);
    if (cbr_w)
        cbr_w(cbr_ctx);

    return dst;
}

/*
 * D8 native copies are already torn down before cbr_w() is invoked.
 * Generic mlv_lite calls this from its write callback; no additional work
 * remains here.
 */
void edmac_copy_rectangle_adv_cleanup(void)
{
}
