/** \file
 * EOS M6 Mark II DIGIC 8 EDMAC memcpy backend for mlv_lite.
 *
 * Use Canon's native MemoryToMemoryEsub5 wrappers recovered from the
 * M6II 1.1.1 ROM.  The same configuration was validated by the v3
 * one-shot 1 MiB RAM copy test.
 */

#include "edmac-memcpy.h"
#include "dryos.h"
#include "edmac.h"
#include "arm-mcr.h"

#define M6II_MEM2MEM_RD_CH 61u
#define M6II_MEM2MEM_WR_CH 24u
#define M6II_MEM2MEM_MODE  1u

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

/*
 * Only one MLV rectangle copy may be active at once; mlv_lite enforces this
 * with edmac_active.  Keep the geometry in static storage so Canon's native
 * path never observes stack-backed descriptors after this function returns.
 */
static struct edmac_info m6ii_src_region;
static struct edmac_info m6ii_dst_region;

static volatile int m6ii_esub5_setup_active = 0;
static volatile int m6ii_copy_done = 0;
static void (*m6ii_user_cbr_r)(void *) = 0;
static void (*m6ii_user_cbr_w)(void *) = 0;
static void *m6ii_user_cbr_ctx = 0;

static void m6ii_finish_previous_copy(void)
{
    if (!m6ii_esub5_setup_active)
        return;

    M6II_ESUB5_CLEANUP();
    M6II_ESUB5_RESET_CBR();
    m6ii_esub5_setup_active = 0;
    m6ii_user_cbr_r = 0;
    m6ii_user_cbr_w = 0;
    m6ii_user_cbr_ctx = 0;
}

/*
 * Canon's Esub5 callback is the actual hardware completion signal.
 * Do not perform path teardown here: this callback may execute in a context
 * where cleanup is unsafe.  Teardown is deferred until the next copy (or
 * resource unlock).  MLV's write callback clears edmac_active immediately.
 */
static void m6ii_copy_done_cbr(void *ctx)
{
    (void)ctx;
    m6ii_copy_done = 1;

    void (*cbr_r)(void *) = m6ii_user_cbr_r;
    void (*cbr_w)(void *) = m6ii_user_cbr_w;
    void *user_ctx = m6ii_user_cbr_ctx;

    if (cbr_r)
        cbr_r(user_ctx);
    if (cbr_w)
        cbr_w(user_ctx);
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
    /*
     * On recorder stop, INT_MIN can follow the final frame immediately.
     * Give the native Esub5 callback time to complete before teardown.
     */
    int wait_ms = 0;
    while (m6ii_esub5_setup_active && !m6ii_copy_done && wait_ms < 1500)
    {
        msleep(1);
        wait_ms++;
    }

    m6ii_finish_previous_copy();

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
     * The previous copy has already signalled completion (otherwise MLV would
     * still have edmac_active set and would not start another one).
     */
    m6ii_finish_previous_copy();

    /* Clean CPU cache before a DMA read from cacheable RAM. */
    if (src != UNCACHEABLE(src))
        sync_caches();

    uint32_t src_adjusted = (uint32_t)src + src_x + src_y * src_width;
    uint32_t dst_adjusted = (uint32_t)dst + dst_x + dst_y * dst_width;

    memset(&m6ii_src_region, 0, sizeof(m6ii_src_region));
    memset(&m6ii_dst_region, 0, sizeof(m6ii_dst_region));

    m6ii_src_region.off1b = src_width - w;
    m6ii_src_region.xb = w;
    m6ii_src_region.yb = h - 1;

    m6ii_dst_region.off1b = dst_width - w;
    m6ii_dst_region.xb = w;
    m6ii_dst_region.yb = h - 1;

    const uint32_t channels[2] = {
        M6II_MEM2MEM_RD_CH,
        M6II_MEM2MEM_WR_CH
    };
    const uint32_t addresses[2] = {
        src_adjusted,
        dst_adjusted
    };
    const uintptr_t size_args[3] = {
        (uintptr_t)&m6ii_src_region,
        (uintptr_t)&m6ii_dst_region,
        M6II_MEM2MEM_MODE
    };

    m6ii_user_cbr_r = cbr_r;
    m6ii_user_cbr_w = cbr_w;
    m6ii_user_cbr_ctx = cbr_ctx;
    m6ii_copy_done = 0;

    M6II_ESUB5_SETUP(channels);
    m6ii_esub5_setup_active = 1;
    M6II_ESUB5_SET_CBR(m6ii_copy_done_cbr, 0);
    M6II_ESUB5_SET_ADDR(addresses);
    M6II_ESUB5_SET_SIZE(size_args);
    M6II_ESUB5_START();

    /* Asynchronous: Canon callback will clear mlv_lite's edmac_active. */
    return dst;
}

void edmac_copy_rectangle_adv_cleanup(void)
{
    /*
     * Called from mlv_lite's write-complete callback.  Native Esub5 teardown
     * is deliberately deferred to the next copy/resource unlock.
     */
}
