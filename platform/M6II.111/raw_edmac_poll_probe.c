/** \file
 * EOS M6 Mark II RAW EDMAC live-window probe.
 *
 * Read-only diagnostic for Bilal-style RAW LV porting.  It samples only the
 * five DIGIC-8 edmac_mmio fields consumed by raw.c, at Canon RAW-engine
 * logical channel 0x4B's ROM-mapped base 0xD04C0300.  No EDMAC writes,
 * redirects, alternate channels or synthetic geometry.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE          0x00010970
#define M6II_RAW_EDMAC_BASE          0xD04C0300
#define M6II_RAW_EDMAC_YS_XS         (M6II_RAW_EDMAC_BASE + 0x48)
#define M6II_RAW_EDMAC_YA_XA         (M6II_RAW_EDMAC_BASE + 0x4C)
#define M6II_RAW_EDMAC_YB_XB         (M6II_RAW_EDMAC_BASE + 0x50)
#define M6II_RAW_EDMAC_YN_XN         (M6II_RAW_EDMAC_BASE + 0x54)
#define M6II_RAW_EDMAC_RAM_ADDR      (M6II_RAW_EDMAC_BASE + 0xA0)
#define M6II_RAW_EDMAC_POLL_MS       750
#define M6II_RAW_EDMAC_POLL_LOG      "M6II_RAW_EDMAC_POLL.LOG"

static volatile int m6ii_raw_edmac_poll_busy = 0;

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t state_read32(uint32_t off)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + off);
}

struct observed_field
{
    uint32_t first;
    uint32_t last;
    uint32_t nonzero_reads;
    uint32_t first_ms;
};

static inline void observe_field(struct observed_field *o, uint32_t v, uint32_t elapsed)
{
    if (v)
    {
        if (!o->nonzero_reads)
        {
            o->first = v;
            o->first_ms = elapsed;
        }
        o->last = v;
        o->nonzero_reads++;
    }
}

static void m6ii_raw_edmac_poll_probe()
{
    if (m6ii_raw_edmac_poll_busy)
    {
        NotifyBox(2000, "RAW EDMAC poll already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW EDMAC poll requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW EDMAC poll");
        return;
    }

    m6ii_raw_edmac_poll_busy = 1;

    struct observed_field ys = {0};
    struct observed_field ya = {0};
    struct observed_field yb = {0};
    struct observed_field yn = {0};
    struct observed_field ra = {0};

    uint32_t samples = 0;
    uint32_t state_w_first = 0;
    uint32_t state_h_first = 0;
    uint32_t state_buf_first = 0;
    uint32_t state_buf_last = 0;

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);

    uint32_t start = get_ms_clock();
    uint32_t elapsed = 0;

    /*
     * Intentionally no sleep here.  Canon may program/reset these registers
     * inside a short frame window.  DryOS still preempts this normal task;
     * we simply sample the one proven ROM-mapped block whenever scheduled.
     */
    do
    {
        elapsed = get_ms_clock() - start;

        observe_field(&ys, mmio_read32(M6II_RAW_EDMAC_YS_XS), elapsed);
        observe_field(&ya, mmio_read32(M6II_RAW_EDMAC_YA_XA), elapsed);
        observe_field(&yb, mmio_read32(M6II_RAW_EDMAC_YB_XB), elapsed);
        observe_field(&yn, mmio_read32(M6II_RAW_EDMAC_YN_XN), elapsed);
        observe_field(&ra, mmio_read32(M6II_RAW_EDMAC_RAM_ADDR), elapsed);

        uint32_t sw = state_read32(0x10);
        uint32_t sh = state_read32(0x14);
        uint32_t sb = state_read32(0x58);
        if (!state_w_first && sw) state_w_first = sw;
        if (!state_h_first && sh) state_h_first = sh;
        if (!state_buf_first && sb) state_buf_first = sb;
        if (sb) state_buf_last = sb;

        samples++;
    }
    while (elapsed < M6II_RAW_EDMAC_POLL_MS);

    int ret_off = call("lv_save_raw", 0);

    FILE *f = FIO_CreateFile(M6II_RAW_EDMAC_POLL_LOG);
    if (f)
    {
        char line[1400];
        int len = snprintf(line, sizeof(line),
            "M6II RAW EDMAC live-window poll\n"
            "edmac_base=0x%08x\n"
            "poll_ms=%u\n"
            "samples=%u\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "state_width_first=0x%08x\n"
            "state_height_first=0x%08x\n"
            "state_buffer_first=0x%08x\n"
            "state_buffer_last=0x%08x\n"
            "ys_xs_first=0x%08x\n"
            "ys_xs_last=0x%08x\n"
            "ys_xs_nonzero_reads=%u\n"
            "ys_xs_first_ms=%u\n"
            "ya_xa_first=0x%08x\n"
            "ya_xa_last=0x%08x\n"
            "ya_xa_nonzero_reads=%u\n"
            "ya_xa_first_ms=%u\n"
            "yb_xb_first=0x%08x\n"
            "yb_xb_last=0x%08x\n"
            "yb_xb_nonzero_reads=%u\n"
            "yb_xb_first_ms=%u\n"
            "yn_xn_first=0x%08x\n"
            "yn_xn_last=0x%08x\n"
            "yn_xn_nonzero_reads=%u\n"
            "yn_xn_first_ms=%u\n"
            "ram_addr_first=0x%08x\n"
            "ram_addr_last=0x%08x\n"
            "ram_addr_nonzero_reads=%u\n"
            "ram_addr_first_ms=%u\n"
            "lv_save_raw_0=0x%08x\n"
            "read_only_mmio=1\n",
            M6II_RAW_EDMAC_BASE,
            M6II_RAW_EDMAC_POLL_MS,
            samples,
            ret_mm, ret_on,
            state_w_first, state_h_first,
            state_buf_first, state_buf_last,
            ys.first, ys.last, ys.nonzero_reads, ys.first_ms,
            ya.first, ya.last, ya.nonzero_reads, ya.first_ms,
            yb.first, yb.last, yb.nonzero_reads, yb.first_ms,
            yn.first, yn.last, yn.nonzero_reads, yn.first_ms,
            ra.first, ra.last, ra.nonzero_reads, ra.first_ms,
            ret_off);

        FIO_WriteFile(f, line, len);
        FIO_CloseFile(f);
    }

    DryosDebugMsg(0, 15,
        "M6II RAW EDMAC poll: samples=%u yb=%08x/%u addr=%08x/%u state=%ux%u %08x",
        samples, yb.first, yb.nonzero_reads, ra.first, ra.nonzero_reads,
        state_w_first, state_h_first, state_buf_first);

    m6ii_raw_edmac_poll_busy = 0;

    NotifyBox(7000,
        "RAW EDMAC poll: yb=%08x (%u) addr=%08x (%u); upload M6II_RAW_EDMAC_POLL.LOG",
        yb.first, yb.nonzero_reads, ra.first, ra.nonzero_reads);
}

static struct menu_entry m6ii_raw_edmac_poll_menu[] = {
    {
        .name   = "M6II RAW EDMAC live poll",
        .priv   = m6ii_raw_edmac_poll_probe,
        .select = run_in_separate_task,
        .help   = "Read-only 750 ms tight poll of Bilal RAW EDMAC fields at Canon ch 0x4B / D04C0300."
    },
};

static void m6ii_raw_edmac_poll_init()
{
    menu_add("Debug", m6ii_raw_edmac_poll_menu, COUNT(m6ii_raw_edmac_poll_menu));
}

INIT_FUNC(__FILE__, m6ii_raw_edmac_poll_init);

#endif /* !CONFIG_HELLO_WORLD */
