/** \file
 * M6II.111 DIGIC 8 RAW EDMAC census.
 *
 * Read-only diagnostic.  This reproduces the discovery method used on the
 * early DIGIC 8 ports: wake the EDMAC power domains, snapshot every DmacInfo
 * channel with Canon RAW disabled, enable lv_save_raw(), poll every channel
 * while RAW is live, then snapshot again after RAW is disabled.
 *
 * No EDMAC register is written and no buffer is redirected.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE       0x00010970u
#define M6II_DMACINFO_BASE        0xE1008944u
#define M6II_EDMAC_CHANNELS       76u

#define M6II_CENSUS_LOG           "M6II_EDMAC_CENSUS.LOG"
#define M6II_RAW_SETTLE_MS        50u
#define M6II_CENSUS_POLL_MS       500u
#define M6II_CENSUS_STEP_MS       5u

/* D8 edmac_mmio fields relevant to Bilal/Kitor raw.c discovery.
 * +0xAC is also logged because M6II has Canon paths which use an address-like
 * field there rather than the traditional +0xA0 ram_addr field.
 */
static const uint16_t census_offsets[] = {
    0x48u, /* ys_xs */
    0x4Cu, /* ya_xa */
    0x50u, /* yb_xb */
    0x54u, /* yn_xn */
    0xA0u, /* ram_addr in ML's D8 edmac_mmio */
    0xACu, /* alternate address-like field seen in M6II Canon paths */
};

#define CENSUS_FIELDS COUNT(census_offsets)

struct field_stat
{
    uint32_t off;
    uint32_t first;
    uint32_t last;
    uint32_t or_all;
    uint32_t post;
    uint32_t nonzero;
    uint32_t changes;
};

struct channel_stat
{
    uint32_t base;
    uint32_t mode;
    uint32_t valid;
    struct field_stat f[CENSUS_FIELDS];
};

static struct channel_stat census[M6II_EDMAC_CHANNELS];
static volatile int m6ii_census_busy = 0;

/* D8 PwrMng lists are terminated by 7.  Holding our wake reference for the
 * entire RAW-on interval avoids suspending a domain while Canon still uses it.
 */
static const uint32_t all_edmac_subchips[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };

extern void PwrMng_WakeSubChips(const uint32_t *list);
extern void PwrMng_SuspendSubChips(const uint32_t *list);

static inline uint32_t read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t dmacinfo_word(uint32_t channel, uint32_t word)
{
    return read32(M6II_DMACINFO_BASE + channel * 8u + word * 4u);
}

static inline int valid_mmio_base(uint32_t base)
{
    return ((base >> 28) == 0xDu) && ((base & 0xFFu) == 0);
}

static void load_channel_table(void)
{
    memset(census, 0, sizeof(census));

    for (uint32_t ch = 0; ch < M6II_EDMAC_CHANNELS; ch++)
    {
        census[ch].base = dmacinfo_word(ch, 0);
        census[ch].mode = dmacinfo_word(ch, 1);
        census[ch].valid = valid_mmio_base(census[ch].base);
    }
}

static void snapshot_off(void)
{
    for (uint32_t ch = 0; ch < M6II_EDMAC_CHANNELS; ch++)
    {
        if (!census[ch].valid)
            continue;

        for (uint32_t j = 0; j < CENSUS_FIELDS; j++)
            census[ch].f[j].off = read32(census[ch].base + census_offsets[j]);
    }
}

static void snapshot_post(void)
{
    for (uint32_t ch = 0; ch < M6II_EDMAC_CHANNELS; ch++)
    {
        if (!census[ch].valid)
            continue;

        for (uint32_t j = 0; j < CENSUS_FIELDS; j++)
            census[ch].f[j].post = read32(census[ch].base + census_offsets[j]);
    }
}

static void poll_raw_on(void)
{
    uint32_t sample = 0;
    uint32_t elapsed = 0;

    while (1)
    {
        for (uint32_t ch = 0; ch < M6II_EDMAC_CHANNELS; ch++)
        {
            if (!census[ch].valid)
                continue;

            for (uint32_t j = 0; j < CENSUS_FIELDS; j++)
            {
                struct field_stat *s = &census[ch].f[j];
                uint32_t v = read32(census[ch].base + census_offsets[j]);

                if (sample == 0)
                {
                    s->first = v;
                }
                else if (v != s->last)
                {
                    s->changes++;
                }

                s->last = v;
                s->or_all |= v;
                if (v)
                    s->nonzero++;
            }
        }

        sample++;
        if (elapsed >= M6II_CENSUS_POLL_MS)
            break;

        msleep(M6II_CENSUS_STEP_MS);
        elapsed += M6II_CENSUS_STEP_MS;
        if (elapsed > M6II_CENSUS_POLL_MS)
            elapsed = M6II_CENSUS_POLL_MS;
    }
}

static uint32_t channel_delta_fields(const struct channel_stat *c)
{
    uint32_t n = 0;

    for (uint32_t j = 0; j < CENSUS_FIELDS; j++)
    {
        const struct field_stat *s = &c->f[j];

        if (s->first != s->off ||
            s->last  != s->off ||
            s->post  != s->off ||
            s->changes ||
            s->nonzero)
        {
            n++;
        }
    }

    return n;
}

static char channel_dir(uint32_t mode)
{
    /* DIGIC 8 DmacInfo ModeInfo: bit 0 = write, bit 1 = read. */
    if (mode & 0x1u) return 'W';
    if (mode & 0x2u) return 'R';
    return '?';
}

static void write_census_log(
    int ret_preoff,
    int ret_mm,
    int ret_on,
    int ret_off,
    uint32_t state_w,
    uint32_t state_h,
    uint32_t state_buf)
{
    FILE *f = FIO_CreateFile(M6II_CENSUS_LOG);
    if (!f)
        return;

    char line[768];

    int len = snprintf(line, sizeof(line),
        "M6II DIGIC8 RAW EDMAC census v1\n"
        "dmacinfo_base=0x%08x\n"
        "channels=%u\n"
        "raw_settle_ms=%u\n"
        "poll_ms=%u\n"
        "poll_step_ms=%u\n"
        "lv_save_raw_preoff=0x%08x\n"
        "lv_set_mm_1=0x%08x\n"
        "lv_save_raw_1=0x%08x\n"
        "state_width=0x%08x\n"
        "state_height=0x%08x\n"
        "state_buffer=0x%08x\n"
        "lv_save_raw_0=0x%08x\n"
        "mmio_writes=0\n"
        "fields=48:ys_xs,4c:ya_xa,50:yb_xb,54:yn_xn,a0:ram_addr,ac:alt_addr\n"
        "mode_bits=bit0:WRITE,bit1:READ\n\n",
        M6II_DMACINFO_BASE,
        M6II_EDMAC_CHANNELS,
        M6II_RAW_SETTLE_MS,
        M6II_CENSUS_POLL_MS,
        M6II_CENSUS_STEP_MS,
        ret_preoff,
        ret_mm,
        ret_on,
        state_w,
        state_h,
        state_buf,
        ret_off);

    FIO_WriteFile(f, line, len);

    for (uint32_t ch = 0; ch < M6II_EDMAC_CHANNELS; ch++)
    {
        struct channel_stat *c = &census[ch];
        uint32_t delta = c->valid ? channel_delta_fields(c) : 0;

        len = snprintf(line, sizeof(line),
            "CH %02u base=%08x mode=%08x dir=%c valid=%u delta_fields=%u\n",
            ch, c->base, c->mode, channel_dir(c->mode), c->valid, delta);
        FIO_WriteFile(f, line, len);

        if (!c->valid)
            continue;

        for (uint32_t j = 0; j < CENSUS_FIELDS; j++)
        {
            struct field_stat *s = &c->f[j];
            len = snprintf(line, sizeof(line),
                "  +%02x off=%08x first=%08x last=%08x or=%08x post=%08x nz=%u changes=%u\n",
                census_offsets[j],
                s->off,
                s->first,
                s->last,
                s->or_all,
                s->post,
                s->nonzero,
                s->changes);
            FIO_WriteFile(f, line, len);
        }
    }

    FIO_CloseFile(f);
}

static void m6ii_raw_edmac_census(void)
{
    if (m6ii_census_busy)
    {
        NotifyBox(2000, "EDMAC census already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "EDMAC census requires LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_census_busy = 1;

    load_channel_table();

    /* Establish a known RAW-off baseline before touching any EDMAC MMIO. */
    int ret_preoff = call("lv_save_raw", 0);
    msleep(M6II_RAW_SETTLE_MS);

    /* Wake every D8 EDMAC domain once and keep that reference until RAW has
     * been turned off again.  This is the important anti-hard-lock step.
     */
    PwrMng_WakeSubChips(all_edmac_subchips);

    snapshot_off();

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);
    msleep(M6II_RAW_SETTLE_MS);

    uint32_t state_w   = read32(M6II_RAW_STATE_BASE + 0x10u);
    uint32_t state_h   = read32(M6II_RAW_STATE_BASE + 0x14u);
    uint32_t state_buf = read32(M6II_RAW_STATE_BASE + 0x58u);

    poll_raw_on();

    int ret_off = call("lv_save_raw", 0);
    msleep(M6II_RAW_SETTLE_MS);

    snapshot_post();

    PwrMng_SuspendSubChips(all_edmac_subchips);

    /* File I/O only after RAW is off and our EDMAC power references are gone. */
    write_census_log(ret_preoff, ret_mm, ret_on, ret_off,
                     state_w, state_h, state_buf);

    m6ii_census_busy = 0;
    NotifyBox(6000, "EDMAC census done; upload %s", M6II_CENSUS_LOG);
}

static struct menu_entry m6ii_raw_census_menu[] = {
    {
        .name   = "M6II RAW EDMAC census",
        .priv   = m6ii_raw_edmac_census,
        .select = run_in_separate_task,
        .help   = "Read-only RAW-off/on census of all 76 DIGIC 8 EDMAC channels."
    },
};

static void m6ii_raw_census_init(void)
{
    menu_add("Debug", m6ii_raw_census_menu, COUNT(m6ii_raw_census_menu));
    DryosDebugMsg(0, 15, "M6II RAW EDMAC census registered");
}

INIT_FUNC(__FILE__, m6ii_raw_census_init);

#endif
