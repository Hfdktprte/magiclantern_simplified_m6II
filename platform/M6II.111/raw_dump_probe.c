/** \file
 * EOS M6 Mark II Canon RAW dump probe.
 *
 * Uses Canon's own lv_raw_dump eventproc while the already-proven RAW LV path
 * is active. This is intended to reveal Canon's current RAW buffer address and
 * dump size without touching any guessed EDMAC/MMIO register.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

static volatile int m6ii_raw_dump_busy = 0;

static void m6ii_canon_raw_dump_once()
{
    if (m6ii_raw_dump_busy)
    {
        NotifyBox(2000, "Canon RAW dump already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW dump requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop recording before RAW dump");
        return;
    }

    m6ii_raw_dump_busy = 1;
    DryosDebugMsg(0, 15, "M6II RAW dump: BEGIN");

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);

    /* Give Canon's LV pipeline a few frames to settle before asking its own
     * debug helper to dump the current RAW buffer. */
    msleep(250);

    NotifyBox(3000, "Canon lv_raw_dump running; do not power off");
    int ret_dump = call("lv_raw_dump");

    int ret_off = call("lv_save_raw", 0);

    DryosDebugMsg(0, 15,
        "M6II RAW dump: END mm=0x%x on=0x%x dump=0x%x off=0x%x",
        ret_mm, ret_on, ret_dump, ret_off);

    m6ii_raw_dump_busy = 0;
    NotifyBox(6000,
        "Canon RAW dump done. Power off, then send new root/ML LOGS files.");
}

static struct menu_entry m6ii_raw_dump_menu[] = {
    {
        .name   = "Canon lv_raw_dump ONCE",
        .priv   = m6ii_canon_raw_dump_once,
        .select = run_in_separate_task,
        .help   = "Stage 2d: Canon's own RAW dump helper. No guessed EDMAC/MMIO access."
    },
};

static void m6ii_raw_dump_probe_init()
{
    menu_add("Debug", m6ii_raw_dump_menu, COUNT(m6ii_raw_dump_menu));
    DryosDebugMsg(0, 15, "M6II RAW dump probe: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_dump_probe_init);

#endif /* !CONFIG_HELLO_WORLD */
