/** \file
 * EOS M6 Mark II ROM-derived RAW LiveView state snapshot.
 *
 * The normal state probe reads ordinary RAM fields identified from this
 * camera's own M6II.111 LiveView code.  The EDMAC probe additionally performs
 * a very narrow, read-only snapshot of the exact DIGIC 8 fields consumed by
 * Bilal's raw_lv_get_resolution()/raw_get_default_lv_buffer() path.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE          0x00010970
#define M6II_RAW_STATE_LOG           "M6II_RAW_STATE.LOG"
#define M6II_RAW_EDMAC_LOG           "M6II_RAW_EDMAC.LOG"
#define M6II_RAW_STATE_OBSERVE_MS    500

/* Canon logical RAW-engine destination channel 0x4B. */
#define M6II_RAW_EDMAC_BASE          0xD04C0300

/* Exact DIGIC-8 struct edmac_mmio offsets used by Bilal's raw.c. */
#define M6II_RAW_EDMAC_YS_XS         (M6II_RAW_EDMAC_BASE + 0x48)
#define M6II_RAW_EDMAC_YA_XA         (M6II_RAW_EDMAC_BASE + 0x4C)
#define M6II_RAW_EDMAC_YB_XB         (M6II_RAW_EDMAC_BASE + 0x50)
#define M6II_RAW_EDMAC_YN_XN         (M6II_RAW_EDMAC_BASE + 0x54)
#define M6II_RAW_EDMAC_RAM_ADDR      (M6II_RAW_EDMAC_BASE + 0xA0)

/*
 * Proven from the uploaded runtime RAM code:
 *
 * 0x02285a6e computes ((state[0x10] * state[0x14]) * 7) / 4.
 * 0x02285a80 returns state + 0x10.
 * 0x02285a86 returns state + 0x14.
 * 0x02285a90 returns state + 0x58.
 * 0x02285a96 stores lv_save_raw argument at state + 0x40.
 * 0x02285a9c returns state + 0x44 (lv_set_mm argument).
 *
 * 7/4 bytes per pixel is 14-bit packed RAW.
 */

static volatile int m6ii_raw_state_busy = 0;

static uint32_t m6ii_raw_state_read(uint32_t byte_offset)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + byte_offset);
}

static uint32_t m6ii_raw_edmac_read(uint32_t address)
{
    return *(volatile uint32_t *)address;
}

static void m6ii_raw_state_snapshot()
{
    if (m6ii_raw_state_busy)
    {
        NotifyBox(2000, "RAW state snapshot already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW state snapshot requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW state snapshot");
        return;
    }

    m6ii_raw_state_busy = 1;
    DryosDebugMsg(0, 15, "M6II RAW state: BEGIN");

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);

    NotifyBox(M6II_RAW_STATE_OBSERVE_MS, "RAW state: observing");
    msleep(M6II_RAW_STATE_OBSERVE_MS);

    uint32_t width_or_pitch = m6ii_raw_state_read(0x10);
    uint32_t height_or_lines = m6ii_raw_state_read(0x14);
    uint32_t raw_on = m6ii_raw_state_read(0x40);
    uint32_t raw_type = m6ii_raw_state_read(0x44);
    uint32_t raw_buffer = m6ii_raw_state_read(0x58);
    uint32_t raw_buffer_prev = m6ii_raw_state_read(0x5c);
    uint32_t buffer_seen = m6ii_raw_state_read(0x60);

    uint32_t frame_size = 0;
    if (width_or_pitch && height_or_lines &&
        width_or_pitch < 0x10000 && height_or_lines < 0x10000)
    {
        frame_size = (width_or_pitch * height_or_lines * 7) / 4;
    }

    /* Release RAW before doing any file I/O. */
    int ret_off = call("lv_save_raw", 0);

    DryosDebugMsg(0, 15,
        "M6II RAW state: mm=%x on=%x off=%x w=%u h=%u buf=%08x prev=%08x size=%u",
        ret_mm, ret_on, ret_off, width_or_pitch, height_or_lines,
        raw_buffer, raw_buffer_prev, frame_size);

    FILE *f = FIO_CreateFile(M6II_RAW_STATE_LOG);
    if (f)
    {
        char line[512];
        int len = snprintf(line, sizeof(line),
            "M6II RAW state snapshot\n"
            "observe_ms=%d\n"
            "state_base=0x%08x\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "state_10=0x%08x\n"
            "state_14=0x%08x\n"
            "raw_on_40=0x%08x\n"
            "raw_type_44=0x%08x\n"
            "raw_buffer_58=0x%08x\n"
            "raw_buffer_prev_5c=0x%08x\n"
            "buffer_seen_60=0x%08x\n"
            "frame_size_14bit=0x%08x\n"
            "frame_size_14bit_dec=%u\n"
            "lv_save_raw_0=0x%08x\n"
            "raw_released_before_file_io=1\n",
            M6II_RAW_STATE_OBSERVE_MS,
            M6II_RAW_STATE_BASE,
            ret_mm,
            ret_on,
            width_or_pitch,
            height_or_lines,
            raw_on,
            raw_type,
            raw_buffer,
            raw_buffer_prev,
            buffer_seen,
            frame_size,
            frame_size,
            ret_off);
        FIO_WriteFile(f, line, len);
        FIO_CloseFile(f);
    }

    m6ii_raw_state_busy = 0;

    NotifyBox(5000, "RAW state: %ux%u buf=%08x size=%u; upload M6II_RAW_STATE.LOG",
              width_or_pitch, height_or_lines, raw_buffer, frame_size);
}

static void m6ii_raw_edmac_snapshot()
{
    if (m6ii_raw_state_busy)
    {
        NotifyBox(2000, "RAW probe already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW EDMAC snapshot requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW EDMAC snapshot");
        return;
    }

    m6ii_raw_state_busy = 1;

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);
    msleep(M6II_RAW_STATE_OBSERVE_MS);

    /* Canon RAM state, captured at the same instant as the EDMAC fields. */
    uint32_t state_width = m6ii_raw_state_read(0x10);
    uint32_t state_height = m6ii_raw_state_read(0x14);
    uint32_t state_buffer = m6ii_raw_state_read(0x58);

    /* Exact five fields relevant to Bilal's RAW-LV geometry/buffer path. */
    uint32_t ys_xs = m6ii_raw_edmac_read(M6II_RAW_EDMAC_YS_XS);
    uint32_t ya_xa = m6ii_raw_edmac_read(M6II_RAW_EDMAC_YA_XA);
    uint32_t yb_xb = m6ii_raw_edmac_read(M6II_RAW_EDMAC_YB_XB);
    uint32_t yn_xn = m6ii_raw_edmac_read(M6II_RAW_EDMAC_YN_XN);
    uint32_t ram_addr = m6ii_raw_edmac_read(M6II_RAW_EDMAC_RAM_ADDR);

    int bilal_ok = (yb_xb != 0);
    uint32_t bilal_pitch = yb_xb & 0xffff;
    uint32_t bilal_width = bilal_pitch * 8 / 14;
    uint32_t bilal_height_a = (yn_xn & 0xffff) + 1;
    uint32_t bilal_height_b = ((yb_xb >> 16) & 0xffff) + 1;
    uint32_t bilal_height = MAX(bilal_height_a, bilal_height_b);

    /* Release RAW before file I/O. */
    int ret_off = call("lv_save_raw", 0);

    FILE *f = FIO_CreateFile(M6II_RAW_EDMAC_LOG);
    if (f)
    {
        char line[768];
        int len = snprintf(line, sizeof(line),
            "M6II Bilal RAW EDMAC snapshot\n"
            "edmac_base=0x%08x\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "state_width_10=0x%08x\n"
            "state_height_14=0x%08x\n"
            "state_buffer_58=0x%08x\n"
            "ys_xs_48=0x%08x\n"
            "ya_xa_4c=0x%08x\n"
            "yb_xb_50=0x%08x\n"
            "yn_xn_54=0x%08x\n"
            "ram_addr_a0=0x%08x\n"
            "bilal_ok=%u\n"
            "bilal_pitch=%u\n"
            "bilal_width=%u\n"
            "bilal_height=%u\n"
            "lv_save_raw_0=0x%08x\n"
            "read_only_mmio=1\n",
            M6II_RAW_EDMAC_BASE,
            ret_mm, ret_on,
            state_width, state_height, state_buffer,
            ys_xs, ya_xa, yb_xb, yn_xn, ram_addr,
            bilal_ok, bilal_pitch, bilal_width, bilal_height,
            ret_off);
        FIO_WriteFile(f, line, len);
        FIO_CloseFile(f);
    }

    DryosDebugMsg(0, 15,
        "M6II RAW EDMAC: ybxb=%08x ynxn=%08x addr=%08x -> %ux%u",
        yb_xb, yn_xn, ram_addr, bilal_width, bilal_height);

    m6ii_raw_state_busy = 0;

    NotifyBox(6000,
        "RAW EDMAC ybxb=%08x addr=%08x -> %ux%u; upload M6II_RAW_EDMAC.LOG",
        yb_xb, ram_addr, bilal_width, bilal_height);
}

static struct menu_entry m6ii_raw_state_menu[] = {
    {
        .name   = "M6II RAW state snapshot",
        .priv   = m6ii_raw_state_snapshot,
        .select = run_in_separate_task,
        .help   = "ROM-derived RAW state: 500 ms RAW pulse, RAM only."
    },
    {
        .name   = "M6II Bilal RAW EDMAC snapshot",
        .priv   = m6ii_raw_edmac_snapshot,
        .select = run_in_separate_task,
        .help   = "Read-only: sample only D04C0300 fields Bilal raw.c uses while lv_save_raw(1) is active."
    },
};

static void m6ii_raw_state_init()
{
    menu_add("Debug", m6ii_raw_state_menu, COUNT(m6ii_raw_state_menu));
    DryosDebugMsg(0, 15, "M6II RAW state/EDMAC snapshot: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_state_init);

#endif /* !CONFIG_HELLO_WORLD */
