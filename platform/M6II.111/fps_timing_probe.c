/** \file
 * EOS M6 Mark II DIGIC 8 FPS timing probe.
 *
 * Read-only diagnostic for identifying the sensor timing registers used by
 * fps-engio.  Bilal's M50 port uses:
 *   confirm/default latch : 0xD0406190
 *   timer A               : 0xD0406198
 *   timer A default       : 0xD040619C
 *   timer B               : 0xD04061A4
 *
 * M6II values are NOT assumed to be identical.  This probe reads only a
 * narrow group of words around that known DIGIC 8 timing block and logs them.
 * It never writes MMIO and refuses to run while recording.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_FPS_PROBE_BASE 0xD0406190u
#define M6II_FPS_PROBE_WORDS 8u

static inline uint32_t m6ii_fps_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void m6ii_fps_probe(void)
{
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "FPS probe requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before FPS probe");
        return;
    }

    uint32_t first[M6II_FPS_PROBE_WORDS];
    uint32_t second[M6II_FPS_PROBE_WORDS];

    for (uint32_t i = 0; i < M6II_FPS_PROBE_WORDS; i++)
        first[i] = m6ii_fps_read32(M6II_FPS_PROBE_BASE + i * 4u);

    msleep(100);

    for (uint32_t i = 0; i < M6II_FPS_PROBE_WORDS; i++)
        second[i] = m6ii_fps_read32(M6II_FPS_PROBE_BASE + i * 4u);

    char filename[64];
    snprintf(filename, sizeof(filename), "M6II_FPS_R%d_F%d.LOG",
             video_mode_resolution, video_mode_fps);

    FILE *f = FIO_CreateFile(filename);
    if (!f)
    {
        NotifyBox(3000, "Could not create %s", filename);
        return;
    }

    char line[256];
    int len = snprintf(line, sizeof(line),
        "M6II DIGIC8 FPS timing probe v1\n"
        "read_only=1\n"
        "base=0x%08x\n"
        "video_mode_fps=%d\n"
        "video_mode_resolution=%d\n"
        "video_mode_crop=%d\n"
        "video_system_pal=%d\n"
        "sample_gap_ms=100\n\n",
        M6II_FPS_PROBE_BASE,
        video_mode_fps,
        video_mode_resolution,
        video_mode_crop,
        video_system_pal);
    FIO_WriteFile(f, line, len);

    for (uint32_t i = 0; i < M6II_FPS_PROBE_WORDS; i++)
    {
        uint32_t addr = M6II_FPS_PROBE_BASE + i * 4u;
        len = snprintf(line, sizeof(line),
            "%08x first=%08x second=%08x low16=%04x low16_plus1=%u changed=%u\n",
            addr,
            first[i],
            second[i],
            second[i] & 0xffffu,
            (second[i] & 0xffffu) + 1u,
            first[i] != second[i]);
        FIO_WriteFile(f, line, len);
    }

    /*
     * Also print Bilal's M50 interpretation.  This is only a hypothesis for
     * M6II; the log values across several Canon FPS modes will decide whether
     * these offsets are actually the M6II timing pair.
     */
    uint32_t reg_confirm = second[(0xD0406190u - M6II_FPS_PROBE_BASE) / 4u];
    uint32_t reg_a       = second[(0xD0406198u - M6II_FPS_PROBE_BASE) / 4u];
    uint32_t reg_a_def   = second[(0xD040619Cu - M6II_FPS_PROBE_BASE) / 4u];
    uint32_t reg_b       = second[(0xD04061A4u - M6II_FPS_PROBE_BASE) / 4u];

    uint32_t timer_a = (reg_a & 0xffffu) + 1u;
    uint32_t timer_b = (reg_b & 0xffffu) + 1u;
    uint32_t nominal_clock = 0;

    if (video_mode_fps > 0 && timer_a > 1 && timer_b > 1)
        nominal_clock = (uint32_t)video_mode_fps * timer_a * timer_b;

    len = snprintf(line, sizeof(line),
        "\nM50-layout hypothesis:\n"
        "confirm=%08x\n"
        "A=%08x A_default=%08x B=%08x\n"
        "timerA=%u timerB=%u\n"
        "integer_fps_x_A_x_B=%u Hz\n",
        reg_confirm, reg_a, reg_a_def, reg_b,
        timer_a, timer_b, nominal_clock);
    FIO_WriteFile(f, line, len);

    FIO_CloseFile(f);

    NotifyBox(5000, "FPS probe saved: %s", filename);
}

static struct menu_entry m6ii_fps_probe_menu[] = {
    {
        .name   = "M6II FPS timing probe",
        .priv   = m6ii_fps_probe,
        .select = run_in_separate_task,
        .help   = "Read-only snapshot of the DIGIC 8 D0406190 timing block."
    },
};

static void m6ii_fps_probe_init(void *unused)
{
    (void)unused;
    menu_add("Debug", m6ii_fps_probe_menu, COUNT(m6ii_fps_probe_menu));
}

INIT_FUNC(__FILE__, m6ii_fps_probe_init);

#endif
