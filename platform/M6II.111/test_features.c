/** \file
 * EOS M6 Mark II RAW bring-up probes.
 *
 * Deliberately small and conservative: this file does not enable
 * CONFIG_RAW_LIVEVIEW, redirect EDMAC, allocate RAW frame buffers, or write
 * video.  Stage 1 proves that the Canon DIGIC 8 RAW LiveView path can be
 * entered and left reliably. Stage 2 captures software-visible evidence
 * around that already-proven pulse without touching EDMAC MMIO.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_PULSE_MS 500
#define M6II_RAW_STAGE2_LOG "M6II_RAW_STAGE2.LOG"

static volatile int m6ii_raw_probe_busy = 0;
static volatile int m6ii_raw_probe_requested = 0;

static void m6ii_raw_force_off()
{
    /*
     * raw.c uses lv_save_raw(0) when releasing the DIGIC 8 RAW path.
     * Keep this as a separate recovery command so the user can explicitly
     * request release if a previous experiment returned unexpectedly.
     */
    DryosDebugMsg(0, 15, "M6II RAW probe: force OFF");
    call("lv_save_raw", 0);
    m6ii_raw_probe_requested = 0;
    NotifyBox(2000, "M6II RAW probe: OFF requested");
}

static int m6ii_raw_preflight()
{
    if (m6ii_raw_probe_busy)
    {
        NotifyBox(2000, "M6II RAW probe already running");
        return 0;
    }

    /* Do not experiment while Canon LV is paused/not active. */
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW probe requires active LiveView");
        return 0;
    }

    /* Do not alter the imaging path while Canon or ML is recording. */
    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW probe");
        return 0;
    }

    return 1;
}

static void m6ii_raw_pulse()
{
    if (!m6ii_raw_preflight())
        return;

    m6ii_raw_probe_busy = 1;

    DryosDebugMsg(0, 15, "M6II RAW probe: pulse begin");

    /*
     * DIGIC 8 raw.c sequence:
     *   lv_set_mm(1) selects RAW rather than the default YUV output.
     *   lv_save_raw(1) asks Canon's LV pipeline to expose/save RAW.
     *
     * Do not add EDMAC MMIO access here.  D8 power domains and channel
     * selection must be identified on M6II before touching those registers.
     */
    call("lv_set_mm", 1);
    call("lv_save_raw", 1);
    m6ii_raw_probe_requested = 1;

    NotifyBox(M6II_RAW_PULSE_MS, "M6II RAW probe: ON");
    msleep(M6II_RAW_PULSE_MS);

    /* Match raw_lv_release() behaviour: release lv_save_raw. */
    call("lv_save_raw", 0);
    m6ii_raw_probe_requested = 0;

    DryosDebugMsg(0, 15, "M6II RAW probe: pulse complete");
    NotifyBox(2000, "M6II RAW probe: pulse complete");

    m6ii_raw_probe_busy = 0;
}

static void m6ii_raw_stage2_log()
{
    if (!m6ii_raw_preflight())
        return;

    m6ii_raw_probe_busy = 1;

    /*
     * This stage intentionally observes only software-visible state:
     * eventproc return values plus DryOS' own stored debug messages.
     * It does NOT dereference any guessed EDMAC register or channel address.
     */
    DryosDebugMsg(0, 15, "M6II RAW stage2: BEGIN");

    int ret_mm = call("lv_set_mm", 1);
    DryosDebugMsg(0, 15, "M6II RAW stage2: lv_set_mm(1) -> 0x%x", ret_mm);

    int ret_on = call("lv_save_raw", 1);
    m6ii_raw_probe_requested = 1;
    DryosDebugMsg(0, 15, "M6II RAW stage2: lv_save_raw(1) -> 0x%x", ret_on);

    NotifyBox(M6II_RAW_PULSE_MS, "M6II RAW stage2: observing");
    msleep(M6II_RAW_PULSE_MS);

    int ret_off = call("lv_save_raw", 0);
    m6ii_raw_probe_requested = 0;
    DryosDebugMsg(0, 15, "M6II RAW stage2: lv_save_raw(0) -> 0x%x", ret_off);
    DryosDebugMsg(0, 15, "M6II RAW stage2: END mm=0x%x on=0x%x off=0x%x",
                  ret_mm, ret_on, ret_off);

    /* Write a tiny easy-to-find summary file after RAW has been released. */
    FILE *f = FIO_CreateFile(M6II_RAW_STAGE2_LOG);
    if (f)
    {
        char line[256];
        int len = snprintf(line, sizeof(line),
            "M6II RAW stage2\n"
            "pulse_ms=%d\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "lv_save_raw_0=0x%08x\n"
            "raw_released_before_dumpf=1\n",
            M6II_RAW_PULSE_MS, ret_mm, ret_on, ret_off);
        FIO_WriteFile(f, line, len);
        FIO_CloseFile(f);
    }
    else
    {
        DryosDebugMsg(0, 15, "M6II RAW stage2: could not create %s", M6II_RAW_STAGE2_LOG);
    }

    /*
     * debug.c already uses dumpf for developer diagnostics.  Run it only after
     * lv_save_raw(0), so logging cannot accidentally lengthen the RAW-on window.
     * The dump should contain the BEGIN/return-value/END markers above together
     * with Canon/DryOS messages surrounding the RAW transition.
     */
    int ret_dumpf = call("dumpf");
    DryosDebugMsg(0, 15, "M6II RAW stage2: dumpf -> 0x%x", ret_dumpf);

    m6ii_raw_probe_busy = 0;
    NotifyBox(4000, "Stage2 done: copy M6II_RAW_STAGE2.LOG + newest log*.log");
}

static MENU_UPDATE_FUNC(m6ii_raw_probe_status)
{
    MENU_SET_VALUE("%s%s",
        m6ii_raw_probe_requested ? "RAW requested" : "idle",
        m6ii_raw_probe_busy ? " / busy" : "");
}

static struct menu_entry m6ii_raw_probe_menu[] = {
    {
        .name   = "M6II RAW bring-up",
        .select = menu_open_submenu,
        .help   = "Early DIGIC 8 RAW tests. No recording or EDMAC redirect yet.",
        .submenu_width = 710,
        .children = (struct menu_entry[]) {
            {
                .name   = "RAW pulse (500 ms)",
                .priv   = m6ii_raw_pulse,
                .select = run_in_separate_task,
                .help   = "Stage 1: proven Canon RAW pulse; no logging or EDMAC access."
            },
            {
                .name   = "RAW discovery log (500 ms)",
                .priv   = m6ii_raw_stage2_log,
                .select = run_in_separate_task,
                .help   = "Stage 2: same pulse, records eventproc returns and dumpf logs. No EDMAC MMIO."
            },
            {
                .name   = "Force RAW off",
                .priv   = m6ii_raw_force_off,
                .select = run_in_separate_task,
                .help   = "Recovery action: request lv_save_raw(0)."
            },
            {
                .name      = "Probe state",
                .update    = m6ii_raw_probe_status,
                .icon_type = IT_ALWAYS_ON,
                .help      = "Internal state of this probe only; not an EDMAC/raw_info status check."
            },
            MENU_EOL,
        },
    },
};

static void m6ii_raw_probe_init()
{
    menu_add("Debug", m6ii_raw_probe_menu, COUNT(m6ii_raw_probe_menu));
    DryosDebugMsg(0, 15, "M6II RAW probe: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_probe_init);

#endif /* !CONFIG_HELLO_WORLD */
