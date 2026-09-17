/** \file
 * M6II.111 RAW EDMAC readback proof.
 *
 * Diagnostic only. It does not redirect EDMAC or write any EDMAC register.
 * It reads the proven RAW writer block and a few status offsets Canon itself
 * reads in ROM, while lv_save_raw(1) keeps Canon RAW active.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE     0x00010970
#define M6II_RAW_EDMAC_BASE     0xD04C0300
#define M6II_RAW_READBACK_LOG   "M6II_RAW_READBACK.LOG"
#define M6II_RAW_POLL_MS        750
#define M6II_RAW_POLL_STEP_MS   10

static volatile int m6ii_raw_readback_busy = 0;

static inline uint32_t mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

struct read_stat
{
    uint32_t first;
    uint32_t last;
    uint32_t or_all;
    uint32_t nonzero;
};

static inline void sample_stat(struct read_stat *s, uint32_t v, uint32_t sample)
{
    if (sample == 0) s->first = v;
    s->last = v;
    s->or_all |= v;
    if (v) s->nonzero++;
}

static void m6ii_raw_edmac_readback_proof(void)
{
    if (m6ii_raw_readback_busy)
    {
        NotifyBox(2000, "RAW readback proof already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW readback proof requires LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording first");
        return;
    }

    m6ii_raw_readback_busy = 1;

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);

    struct read_stat ys = {0}, ya = {0}, yb = {0}, yn = {0}, addr = {0};
    struct read_stat ctl04 = {0}, stat_ec = {0}, stat_f0 = {0}, stat_f4 = {0};

    uint32_t samples = 0;
    uint32_t elapsed = 0;

    while (elapsed <= M6II_RAW_POLL_MS)
    {
        sample_stat(&ctl04,  mmio_read(M6II_RAW_EDMAC_BASE + 0x04), samples);
        sample_stat(&ys,     mmio_read(M6II_RAW_EDMAC_BASE + 0x48), samples);
        sample_stat(&ya,     mmio_read(M6II_RAW_EDMAC_BASE + 0x4C), samples);
        sample_stat(&yb,     mmio_read(M6II_RAW_EDMAC_BASE + 0x50), samples);
        sample_stat(&yn,     mmio_read(M6II_RAW_EDMAC_BASE + 0x54), samples);
        sample_stat(&addr,   mmio_read(M6II_RAW_EDMAC_BASE + 0xA0), samples);

        /* Canon ROM has explicit readback helpers for these status offsets. */
        sample_stat(&stat_ec, mmio_read(M6II_RAW_EDMAC_BASE + 0xEC), samples);
        sample_stat(&stat_f0, mmio_read(M6II_RAW_EDMAC_BASE + 0xF0), samples);
        sample_stat(&stat_f4, mmio_read(M6II_RAW_EDMAC_BASE + 0xF4), samples);

        samples++;
        if (elapsed == M6II_RAW_POLL_MS) break;
        msleep(M6II_RAW_POLL_STEP_MS);
        elapsed += M6II_RAW_POLL_STEP_MS;
        if (elapsed > M6II_RAW_POLL_MS) elapsed = M6II_RAW_POLL_MS;
    }

    uint32_t state_w   = *(volatile uint32_t *)(M6II_RAW_STATE_BASE + 0x10);
    uint32_t state_h   = *(volatile uint32_t *)(M6II_RAW_STATE_BASE + 0x14);
    uint32_t state_buf = *(volatile uint32_t *)(M6II_RAW_STATE_BASE + 0x58);

    int ret_off = call("lv_save_raw", 0);

    FILE *f = FIO_CreateFile(M6II_RAW_READBACK_LOG);
    if (f)
    {
        char line[2048];
        int len = snprintf(line, sizeof(line),
            "M6II RAW EDMAC readback proof\n"
            "edmac_base=0x%08x\n"
            "poll_ms=%u\n"
            "samples=%u\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "state_width=0x%08x\n"
            "state_height=0x%08x\n"
            "state_buffer=0x%08x\n"
            "ctl04_first=0x%08x ctl04_last=0x%08x ctl04_or=0x%08x ctl04_nz=%u\n"
            "ys48_first=0x%08x ys48_last=0x%08x ys48_or=0x%08x ys48_nz=%u\n"
            "ya4c_first=0x%08x ya4c_last=0x%08x ya4c_or=0x%08x ya4c_nz=%u\n"
            "yb50_first=0x%08x yb50_last=0x%08x yb50_or=0x%08x yb50_nz=%u\n"
            "yn54_first=0x%08x yn54_last=0x%08x yn54_or=0x%08x yn54_nz=%u\n"
            "addrA0_first=0x%08x addrA0_last=0x%08x addrA0_or=0x%08x addrA0_nz=%u\n"
            "statEC_first=0x%08x statEC_last=0x%08x statEC_or=0x%08x statEC_nz=%u\n"
            "statF0_first=0x%08x statF0_last=0x%08x statF0_or=0x%08x statF0_nz=%u\n"
            "statF4_first=0x%08x statF4_last=0x%08x statF4_or=0x%08x statF4_nz=%u\n"
            "lv_save_raw_0=0x%08x\n"
            "mmio_writes=0\n",
            M6II_RAW_EDMAC_BASE, M6II_RAW_POLL_MS, samples,
            ret_mm, ret_on, state_w, state_h, state_buf,
            ctl04.first, ctl04.last, ctl04.or_all, ctl04.nonzero,
            ys.first, ys.last, ys.or_all, ys.nonzero,
            ya.first, ya.last, ya.or_all, ya.nonzero,
            yb.first, yb.last, yb.or_all, yb.nonzero,
            yn.first, yn.last, yn.or_all, yn.nonzero,
            addr.first, addr.last, addr.or_all, addr.nonzero,
            stat_ec.first, stat_ec.last, stat_ec.or_all, stat_ec.nonzero,
            stat_f0.first, stat_f0.last, stat_f0.or_all, stat_f0.nonzero,
            stat_f4.first, stat_f4.last, stat_f4.or_all, stat_f4.nonzero,
            ret_off);
        FIO_WriteFile(f, line, len);
        FIO_CloseFile(f);
    }

    m6ii_raw_readback_busy = 0;
    NotifyBox(5000, "RAW readback proof done; upload %s", M6II_RAW_READBACK_LOG);
}

static struct menu_entry m6ii_raw_readback_menu[] = {
    {
        .name   = "M6II RAW EDMAC readback proof",
        .priv   = m6ii_raw_edmac_readback_proof,
        .select = run_in_separate_task,
        .help   = "Read-only proof: RAW config vs Canon-readable status registers."
    },
};

static void m6ii_raw_readback_init(void)
{
    menu_add("Debug", m6ii_raw_readback_menu, COUNT(m6ii_raw_readback_menu));
}

INIT_FUNC(__FILE__, m6ii_raw_readback_init);

#endif
