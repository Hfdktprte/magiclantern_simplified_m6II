#include "sd_m6ii.h"

#include <module.h>
#include <dryos.h>
#include <menu.h>

extern int M6II_GetUHS2CardCapability(uint32_t dev, uint32_t *cap);
extern int M6II_IsUhs2Mode(uint32_t drive_letter, uint32_t *is_uhs2);

static volatile int m6ii_sd_test_busy = 0;

static void m6ii_sd_mode_task(void *unused)
{
    uint32_t is_uhs2 = 0xffffffff;

    /*
     * This is the same lock/query/unlock sequence used by Canon's
     * DebugSTG_CheckUHS2Mode eventproc.  Unlike GetUHS2CardCapability,
     * it is specifically intended for an already-mounted B:/ card.
     */
    uint32_t old_int = cli();
    int err = M6II_IsUhs2Mode('B', &is_uhs2);
    sei(old_int);

    DryosDebugMsg(0, 15, "M6II SD: mounted UHS2 mode=%#x err=%d", is_uhs2, err);

    if (err == 0)
    {
        NotifyBox(6000, "SD current mode: %s (%#x)",
                  is_uhs2 ? "UHS-II" : "non UHS-II", is_uhs2);
    }
    else
    {
        NotifyBox(6000, "SD mode query failed: val=%#x err=%d", is_uhs2, err);
    }

    m6ii_sd_test_busy = 0;
}

static void m6ii_sd_sdr_task(void *arg)
{
    int mode = (int)(uintptr_t)arg;

    /*
     * M6II ROM 1.1.1 exposes DebugSTG_SetSDRMode.
     * We intentionally use Canon's own DIGIC 8 storage path here.
     * No DIGIC 5 0xC04006xx register writes are performed.
     */
    int err = call("DebugSTG_SetSDRMode", mode);

    DryosDebugMsg(0, 15, "M6II SD: DebugSTG_SetSDRMode(%d) -> %d", mode, err);
    NotifyBox(6000, "Canon storage mode %d: ret=%d", mode, err);

    m6ii_sd_test_busy = 0;
}

static MENU_SELECT_FUNC(m6ii_sd_read_mode)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_mode", 0x1c, 0x1000, m6ii_sd_mode_task, 0);
}

static void m6ii_sd_start_sdr_mode(int mode)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    NotifyBox(3000, "Testing Canon storage mode %d", mode);
    task_create("m6ii_sd_sdr", 0x1c, 0x1000, m6ii_sd_sdr_task, (void *)(uintptr_t)mode);
}

static MENU_SELECT_FUNC(m6ii_sd_sdr_mode_0)
{
    m6ii_sd_start_sdr_mode(0);
}

static MENU_SELECT_FUNC(m6ii_sd_sdr_mode_1)
{
    m6ii_sd_start_sdr_mode(1);
}

static struct menu_entry m6ii_sd_test_menu[] =
{
    {
        .name = "M6II SD test",
        .select = menu_open_submenu,
        .help = "DIGIC 8 SD tests using Canon's own storage functions.",
        .help2 = "Experimental. Nothing is changed automatically at boot.",
        .children = (struct menu_entry[])
        {
            {
                .name = "Read current UHS-II mode",
                .select = m6ii_sd_read_mode,
                .icon_type = IT_ACTION,
                .help = "Query Canon's mounted-card UHS-II state for B:/.",
                .help2 = "Read-only. Mirrors DebugSTG_CheckUHS2Mode from M6II 1.1.1 ROM.",
            },
            {
                .name = "Canon RecordStart",
                .select = m6ii_sd_sdr_mode_0,
                .icon_type = IT_ACTION,
                .help = "Call DebugSTG_SetSDRMode(0); ROM routes this to RecordStart(B).",
                .help2 = "May reconfigure card access. Reboot if the card becomes unavailable.",
            },
            {
                .name = "Canon RecordEnd",
                .select = m6ii_sd_sdr_mode_1,
                .icon_type = IT_ACTION,
                .help = "Call DebugSTG_SetSDRMode(1); ROM routes this to RecordEnd(B).",
                .help2 = "May reconfigure card access. Reboot if the card becomes unavailable.",
            },
            MENU_EOL,
        },
    },
};

unsigned int init_SD_M6II(void)
{
    if (!is_camera("M6II", "1.1.1"))
        return 0;

    menu_add("Prefs", m6ii_sd_test_menu, COUNT(m6ii_sd_test_menu));
    DryosDebugMsg(0, 15, "M6II SD: native DIGIC 8 test menu loaded");

    return 0;
}
