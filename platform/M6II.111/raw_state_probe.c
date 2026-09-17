/** \file
 * EOS M6 Mark II ROM-derived RAW LiveView state snapshot.
 *
 * This probe reads ordinary RAM fields identified from this camera's own
 * M6II.111 / internal 5.9.2 LiveView code. It does not access EDMAC/MMIO,
 * redirect buffers, allocate SRM, or record video.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE          0x00010970
#define M6II_RAW_STATE_LOG           "M6II_RAW_STATE.LOG"
#define M6II_RAW_STATE_OBSERVE_MS    500

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

static struct menu_entry m6ii_raw_state_menu[] = {
    {
        .name   = "M6II RAW state snapshot",
        .priv   = m6ii_raw_state_snapshot,
        .select = run_in_separate_task,
        .help   = "ROM-derived Stage 2c: 500 ms RAW pulse, read only RAM state, release, then log. No EDMAC MMIO."
    },
};

static void m6ii_raw_state_init()
{
    menu_add("Debug", m6ii_raw_state_menu, COUNT(m6ii_raw_state_menu));
    DryosDebugMsg(0, 15, "M6II RAW state snapshot: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_state_init);

#endif /* !CONFIG_HELLO_WORLD */
