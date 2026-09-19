/** \file
 * EOS M6 Mark II RAW LiveView geometry census.
 *
 * Each Debug-menu invocation records the currently selected Canon movie mode
 * together with two independent views of the active RAW geometry:
 *
 *   1. Canon's ROM-derived RAW state at 0x00010970.
 *   2. The runtime-proven LiveView RAW destination EDMAC at 0xD0420200.
 *
 * The probe never redirects RAW buffers and does not record video.  If RAW
 * LiveView was off when the probe started, it enables it only long enough to
 * let the pipeline settle and collect samples, then releases it before file I/O.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>
#include <timer.h>
#include <edmac.h>

#define M6II_GEOM_RAW_STATE_BASE       0x00010970u
#define M6II_GEOM_RAW_EDMAC_BASE       0xD0420200u

#define M6II_GEOM_SETTLE_MS            500
#define M6II_GEOM_SAMPLE_COUNT         8
#define M6II_GEOM_SAMPLE_INTERVAL_MS   40

/* Verified M6II FPS timing registers; useful for distinguishing Canon readouts. */
#define M6II_GEOM_FPS_A                0xD0406198u
#define M6II_GEOM_FPS_B                0xD04061A4u
#define M6II_GEOM_FPS_CONFIRM          0xD0406190u

struct m6ii_geom_sample
{
    uint32_t state_width;
    uint32_t state_height;
    uint32_t raw_on;
    uint32_t raw_type;
    uint32_t state_50;
    uint32_t state_buffer;
    uint32_t state_buffer_prev;
    uint32_t buffer_seen;
    uint32_t packmode;

    uint32_t edmac_mode;
    uint32_t ys_xs;
    uint32_t ya_xa;
    uint32_t yb_xb;
    uint32_t yn_xn;
    uint32_t edmac_ram_addr;
    uint32_t transfer_mode;
    uint32_t pack_unpack_info;
};

static volatile int m6ii_geom_busy = 0;

static inline uint32_t m6ii_geom_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t m6ii_geom_state(uint32_t off)
{
    return m6ii_geom_read32(M6II_GEOM_RAW_STATE_BASE + off);
}

static int m6ii_geom_bpp_from_packmode(uint32_t packmode)
{
    switch (packmode)
    {
        case 2: return 14;
        case 1: return 12;
        case 0: return 10;
        default: return 0;
    }
}

static void m6ii_geom_take_sample(struct m6ii_geom_sample *s)
{
    volatile struct edmac_mmio *edmac =
        (volatile struct edmac_mmio *)M6II_GEOM_RAW_EDMAC_BASE;

    s->state_width       = m6ii_geom_state(0x10);
    s->state_height      = m6ii_geom_state(0x14);
    s->raw_on            = m6ii_geom_state(0x40);
    s->raw_type          = m6ii_geom_state(0x44);
    s->state_50          = m6ii_geom_state(0x50);
    s->state_buffer      = m6ii_geom_state(0x58);
    s->state_buffer_prev = m6ii_geom_state(0x5c);
    s->buffer_seen       = m6ii_geom_state(0x60);
    s->packmode          = m6ii_geom_state(0x74);

    s->edmac_mode        = edmac->ModeInfo;
    s->ys_xs             = edmac->ys_xs;
    s->ya_xa             = edmac->ya_xa;
    s->yb_xb             = edmac->yb_xb;
    s->yn_xn             = edmac->yn_xn;
    s->edmac_ram_addr    = edmac->ram_addr;
    s->transfer_mode     = edmac->transfer_mode;
    s->pack_unpack_info  = edmac->PackUnpackInfo;
}

static int m6ii_geom_samples_stable(const struct m6ii_geom_sample *samples)
{
    for (int i = 1; i < M6II_GEOM_SAMPLE_COUNT; i++)
    {
        if (samples[i].state_width  != samples[0].state_width  ||
            samples[i].state_height != samples[0].state_height ||
            samples[i].packmode     != samples[0].packmode     ||
            samples[i].yb_xb        != samples[0].yb_xb        ||
            samples[i].yn_xn        != samples[0].yn_xn)
        {
            return 0;
        }
    }
    return 1;
}

static void m6ii_raw_geometry_log()
{
    if (m6ii_geom_busy)
    {
        NotifyBox(2000, "M6II geometry probe already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW geometry requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW geometry probe");
        return;
    }

    m6ii_geom_busy = 1;

    /*
     * Capture Canon mode metadata before touching RAW.  PROP_VIDEO_MODE is the
     * user-facing movie-mode description; it is intentionally logged separately
     * from the Bayer geometry measured below.
     */
    int mode_crop = video_mode_crop;
    int mode_resolution = video_mode_resolution;
    int mode_fps = video_mode_fps;
    int mode_fps_x100 = video_mode_fps_x100;

    uint32_t fps_a = m6ii_geom_read32(M6II_GEOM_FPS_A);
    uint32_t fps_b = m6ii_geom_read32(M6II_GEOM_FPS_B);
    uint32_t fps_confirm = m6ii_geom_read32(M6II_GEOM_FPS_CONFIRM);

    uint32_t raw_was_on = m6ii_geom_state(0x40);
    int ret_mm = 0;
    int ret_on = 0;
    int ret_off = 0;

    if (!raw_was_on)
    {
        ret_mm = call("lv_set_mm", 1);
        ret_on = call("lv_save_raw", 1);
    }

    NotifyBox(M6II_GEOM_SETTLE_MS, "M6II RAW geometry: settling");
    msleep(M6II_GEOM_SETTLE_MS);

    struct m6ii_geom_sample samples[M6II_GEOM_SAMPLE_COUNT];
    memset(samples, 0, sizeof(samples));

    for (int i = 0; i < M6II_GEOM_SAMPLE_COUNT; i++)
    {
        m6ii_geom_take_sample(&samples[i]);
        if (i + 1 < M6II_GEOM_SAMPLE_COUNT)
            msleep(M6II_GEOM_SAMPLE_INTERVAL_MS);
    }

    /*
     * Restore the state we found.  Never perform file I/O while this probe's
     * temporary RAW request is active.
     */
    if (!raw_was_on)
        ret_off = call("lv_save_raw", 0);

    const struct m6ii_geom_sample *s = &samples[M6II_GEOM_SAMPLE_COUNT - 1];

    int bpp = m6ii_geom_bpp_from_packmode(s->packmode);
    uint32_t pitch = s->yb_xb & 0xffffu;
    uint32_t edmac_height_a = (s->yn_xn & 0xffffu) + 1u;
    uint32_t edmac_height_b = ((s->yb_xb >> 16) & 0xffffu) + 1u;
    uint32_t edmac_height = MAX(edmac_height_a, edmac_height_b);
    uint32_t edmac_width = bpp ? (pitch * 8u / (uint32_t)bpp) : 0u;

    uint32_t frame_bytes = 0;
    if (s->state_width && s->state_height && bpp)
        frame_bytes = s->state_width * s->state_height * (uint32_t)bpp / 8u;

    int stable = m6ii_geom_samples_stable(samples);
    int geometry_match =
        bpp &&
        s->state_width == edmac_width &&
        s->state_height == edmac_height;

    /*
     * 8.3-safe unique-ish name: one file per button press. get_ms_clock() is
     * captured only after RAW has been released. Repeating the exact same
     * millisecond after a reboot is possible but extremely unlikely.
     */
    char filename[16];
    snprintf(filename, sizeof(filename), "G%07X.LOG",
             ((uint32_t)get_ms_clock()) & 0x0fffffffu);

    FILE *f = FIO_CreateFile(filename);
    if (!f)
    {
        DryosDebugMsg(0, 15, "M6II geometry: could not create %s", filename);
        m6ii_geom_busy = 0;
        NotifyBox(4000, "RAW geometry %ux%u; log create FAILED",
                  s->state_width, s->state_height);
        return;
    }

    char text[2048];
    int len = snprintf(text, sizeof(text),
        "M6II RAW geometry census\n"
        "log_version=1\n"
        "settle_ms=%d\n"
        "sample_count=%d\n"
        "sample_interval_ms=%d\n"
        "\n"
        "[canon_movie_mode]\n"
        "video_mode_crop=%d\n"
        "video_mode_resolution=%d\n"
        "video_mode_fps=%d\n"
        "video_mode_fps_x100=%d\n"
        "fps_timer_a=0x%08x\n"
        "fps_timer_b=0x%08x\n"
        "fps_confirm=0x%08x\n"
        "\n"
        "[raw_control]\n"
        "raw_was_on=0x%08x\n"
        "lv_set_mm_1=0x%08x\n"
        "lv_save_raw_1=0x%08x\n"
        "lv_save_raw_0=0x%08x\n"
        "\n"
        "[summary]\n"
        "stable=%d\n"
        "geometry_match=%d\n"
        "raw_state_width=%u\n"
        "raw_state_height=%u\n"
        "raw_state_geometry=%ux%u\n"
        "raw_buffer=0x%08x\n"
        "packmode=0x%08x\n"
        "bits_per_pixel=%d\n"
        "edmac_pitch_bytes=%u\n"
        "edmac_width=%u\n"
        "edmac_height=%u\n"
        "edmac_geometry=%ux%u\n"
        "edmac_ram_addr=0x%08x\n"
        "frame_bytes=%u\n"
        "\n"
        "[raw_state_final]\n"
        "state_10_width=0x%08x\n"
        "state_14_height=0x%08x\n"
        "state_40_raw_on=0x%08x\n"
        "state_44_raw_type=0x%08x\n"
        "state_50=0x%08x\n"
        "state_58_buffer=0x%08x\n"
        "state_5c_buffer_prev=0x%08x\n"
        "state_60_buffer_seen=0x%08x\n"
        "state_74_packmode=0x%08x\n"
        "\n"
        "[edmac_final]\n"
        "edmac_base=0x%08x\n"
        "ModeInfo=0x%08x\n"
        "ys_xs=0x%08x\n"
        "ya_xa=0x%08x\n"
        "yb_xb=0x%08x\n"
        "yn_xn=0x%08x\n"
        "transfer_mode=0x%08x\n"
        "PackUnpackInfo=0x%08x\n"
        "ram_addr=0x%08x\n"
        "\n"
        "[samples]\n",
        M6II_GEOM_SETTLE_MS,
        M6II_GEOM_SAMPLE_COUNT,
        M6II_GEOM_SAMPLE_INTERVAL_MS,
        mode_crop,
        mode_resolution,
        mode_fps,
        mode_fps_x100,
        fps_a,
        fps_b,
        fps_confirm,
        raw_was_on,
        ret_mm,
        ret_on,
        ret_off,
        stable,
        geometry_match,
        s->state_width,
        s->state_height,
        s->state_width,
        s->state_height,
        s->state_buffer,
        s->packmode,
        bpp,
        pitch,
        edmac_width,
        edmac_height,
        edmac_width,
        edmac_height,
        s->edmac_ram_addr,
        frame_bytes,
        s->state_width,
        s->state_height,
        s->raw_on,
        s->raw_type,
        s->state_50,
        s->state_buffer,
        s->state_buffer_prev,
        s->buffer_seen,
        s->packmode,
        M6II_GEOM_RAW_EDMAC_BASE,
        s->edmac_mode,
        s->ys_xs,
        s->ya_xa,
        s->yb_xb,
        s->yn_xn,
        s->transfer_mode,
        s->pack_unpack_info,
        s->edmac_ram_addr);

    if (len > 0)
        FIO_WriteFile(f, text, MIN(len, (int)sizeof(text) - 1));

    for (int i = 0; i < M6II_GEOM_SAMPLE_COUNT; i++)
    {
        const struct m6ii_geom_sample *q = &samples[i];
        int sample_bpp = m6ii_geom_bpp_from_packmode(q->packmode);
        uint32_t sample_pitch = q->yb_xb & 0xffffu;
        uint32_t sample_width =
            sample_bpp ? sample_pitch * 8u / (uint32_t)sample_bpp : 0u;
        uint32_t sample_height = MAX(
            (q->yn_xn & 0xffffu) + 1u,
            ((q->yb_xb >> 16) & 0xffffu) + 1u);

        char line[320];
        int line_len = snprintf(line, sizeof(line),
            "sample_%d: state=%ux%u pack=%u bpp=%d "
            "yb_xb=%08x yn_xn=%08x edmac=%ux%u "
            "state_buf=%08x edmac_buf=%08x\n",
            i,
            q->state_width,
            q->state_height,
            q->packmode,
            sample_bpp,
            q->yb_xb,
            q->yn_xn,
            sample_width,
            sample_height,
            q->state_buffer,
            q->edmac_ram_addr);

        if (line_len > 0)
            FIO_WriteFile(f, line, MIN(line_len, (int)sizeof(line) - 1));
    }

    FIO_CloseFile(f);

    DryosDebugMsg(0, 15,
        "M6II geometry: %ux%u state, %ux%u EDMAC, bpp=%d stable=%d match=%d log=%s",
        s->state_width, s->state_height,
        edmac_width, edmac_height,
        bpp, stable, geometry_match, filename);

    m6ii_geom_busy = 0;

    NotifyBox(6000,
        "RAW %ux%u / EDMAC %ux%u %dbit\n%s stable=%d match=%d",
        s->state_width, s->state_height,
        edmac_width, edmac_height,
        bpp, filename, stable, geometry_match);
}

static struct menu_entry m6ii_raw_geometry_menu[] = {
    {
        .name   = "M6II RAW geometry log",
        .priv   = m6ii_raw_geometry_log,
        .select = run_in_separate_task,
        .help   = "Switch Canon movie mode, then press this. Saves RAW state + EDMAC geometry to a new Gxxxxxxx.LOG file."
    },
};

static void m6ii_raw_geometry_init()
{
    menu_add("Debug", m6ii_raw_geometry_menu, COUNT(m6ii_raw_geometry_menu));
    DryosDebugMsg(0, 15, "M6II RAW geometry census: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_geometry_init);

#endif /* !CONFIG_HELLO_WORLD */
