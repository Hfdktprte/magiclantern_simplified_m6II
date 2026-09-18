#include "sd_m6ii.h"

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <fio-ml.h>

extern int M6II_SdCARDGetSpeed(uint32_t dev, uint32_t *speed, uint32_t *clock_selector);

/*
 * M6II 1.1.1 / DIGIC 8 Bilal sd_uhs port notes.
 *
 * Bilal's DIGIC-5 module overrides the SD controller's preset register block
 * inside the normal Canon setup/reconfiguration path.  M6II has the same
 * overall pattern, but with a D8 register block and 11 words per preset.
 *
 * ROM call chain:
 *   SddomChangeClockSpeed -> 0xE0456E1C -> 0xE012BB44
 *   0xE012BB44 selects a preset and calls 0xE012BA6C.
 *   0xE012BA6C writes the 11 words below to the controller.
 *
 * Do not copy the old DIGIC-5 0xC04006xx addresses here.
 */
#define M6II_SD_DEVICE 0

static const uint32_t m6ii_uhs_regs[] =
{
    0xD0100600,
    0xD0100618,
    0xD010061C,
    0xD0100620,
    0xD010062C,
    0xD0100630,
    0xD0100624,
    0xD0100628,
    0xD0100638,
    0xD0100604,
    0xD010060C,
};

/*
 * Preset-table pointers selected by 0xE012BB44 for clock selectors 0..9.
 * Each table contains six 11-word device blocks; device 0 uses the first 11.
 *
 * Selectors 8 and 9 are Canon's 156 MHz and 195 MHz UHS-I presets.
 * We dump every normal selector so the M6II 160/192/240 Bilal-style presets
 * can be derived from Canon's own D8 field layout.
 */
static const uint32_t m6ii_clock_tables[10] =
{
    0x0001E144, /* selector 0 */
    0x0001E1C8, /* selector 1 = 0x1E144 + 0x84 */
    0x0001E24C, /* selector 2 */
    0x0001E2D0, /* selector 3 = 0x1E24C + 0x84 */
    0x0001E354, /* selector 4 */
    0x0001E45C, /* selector 5 */
    0x0001E3D8, /* selector 6 = 0x1E354 + 0x84 */
    0x0001E4E0, /* selector 7 = 0x1E45C + 0x84 */
    0x0001E564, /* selector 8 = 156 MHz */
    0x0001E5E8, /* selector 9 = 195 MHz */
};

static volatile int m6ii_sd_test_busy = 0;

static void m6ii_read_speed_task(void *unused)
{
    uint32_t speed = 0xffffffff;
    uint32_t clock = 0xffffffff;
    int err = M6II_SdCARDGetSpeed(M6II_SD_DEVICE, &speed, &clock);

    DryosDebugMsg(0, 15,
                  "M6II Bilal port: dev=%d speed=%d clock=%d err=%d",
                  M6II_SD_DEVICE, speed, clock, err);

    NotifyBox(7000, "SDR speed=%d clock=%d\ndev=%d err=%d",
              speed, clock, M6II_SD_DEVICE, err);

    m6ii_sd_test_busy = 0;
}

static MENU_SELECT_FUNC(m6ii_read_speed)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD port dump already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_speed", 0x1c, 0x1000, m6ii_read_speed_task, 0);
}

static void m6ii_bilal_dump_task(void *unused)
{
    FILE *f = FIO_CreateFile("ML/LOGS/M6II_UHS.LOG");
    if (!f)
    {
        NotifyBox(5000, "Could not create M6II_UHS.LOG");
        m6ii_sd_test_busy = 0;
        return;
    }

    uint32_t speed = 0xffffffff;
    uint32_t clock = 0xffffffff;
    int err = M6II_SdCARDGetSpeed(M6II_SD_DEVICE, &speed, &clock);

    my_fprintf(f, "M6II 1.1.1 Bilal sd_uhs port dump\n");
    my_fprintf(f, "SdCARDGetSpeed: dev=%d speed=%d clock=%d err=%d\n",
               M6II_SD_DEVICE, speed, clock, err);
    my_fprintf(f, "setup_switch=E012BB44 preset_writer=E012BA6C\n");
    my_fprintf(f, "controller_mode D010F100=%08x\n", MEM(0xD010F100));

    my_fprintf(f, "\nCURRENT D8 UHS REGISTERS\n");
    for (int i = 0; i < COUNT(m6ii_uhs_regs); i++)
    {
        my_fprintf(f, "reg[%02d] %08x=%08x\n",
                   i, m6ii_uhs_regs[i], MEM(m6ii_uhs_regs[i]));
    }

    my_fprintf(f, "\nCANON CLOCK PRESET TABLES (device 0, 11 words)\n");
    for (int sel = 0; sel < COUNT(m6ii_clock_tables); sel++)
    {
        volatile uint32_t *table = (volatile uint32_t *)m6ii_clock_tables[sel];

        my_fprintf(f, "selector=%d table=%08x", sel, m6ii_clock_tables[sel]);
        if (sel == 8)
            my_fprintf(f, " (156MHz)");
        else if (sel == 9)
            my_fprintf(f, " (195MHz stock)");
        my_fprintf(f, "\n");

        for (int i = 0; i < COUNT(m6ii_uhs_regs); i++)
        {
            my_fprintf(f, "  v[%02d]=%08x  -> %08x\n",
                       i, table[i], m6ii_uhs_regs[i]);
        }
    }

    /*
     * Bilal's mechanism ultimately needs a card reconfiguration entry point
     * after installing the setup-mode override.  Keep the known Canon debug
     * pre-init wrapper visible in the log while its exact lifecycle is mapped.
     */
    my_fprintf(f, "\nROM anchors\n");
    my_fprintf(f, "DebugSTG_SDPreInit wrapper=E00BC9B2 target=E057D6D4\n");
    my_fprintf(f, "SddomChangeClockSpeed wrapper=E017879C low=E00F4770\n");
    my_fprintf(f, "D8 preset switch=E012BB44 writer=E012BA6C\n");

    FIO_CloseFile(f);

    DryosDebugMsg(0, 15,
                  "M6II Bilal port: dumped D8 UHS tables to ML/LOGS/M6II_UHS.LOG");
    NotifyBox(8000, "Saved ML/LOGS/M6II_UHS.LOG");

    m6ii_sd_test_busy = 0;
}

static MENU_SELECT_FUNC(m6ii_bilal_dump)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD port dump already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_uhs_dump", 0x1c, 0x1800, m6ii_bilal_dump_task, 0);
}

static struct menu_entry m6ii_bilal_menu[] =
{
    {
        .name = "M6II Bilal SD port",
        .select = menu_open_submenu,
        .help = "Port diagnostics for Bilal's sd_uhs register-override mechanism.",
        .help2 = "Read-only until M6II 160/192/240 D8 presets are verified.",
        .children = (struct menu_entry[])
        {
            {
                .name = "Read speed / clock",
                .select = m6ii_read_speed,
                .icon_type = IT_ACTION,
                .help = "Read Canon's current UHS-I speed enum and clock selector.",
                .help2 = "Stock M6II SDR104 has tested as speed=5, clock=9.",
            },
            {
                .name = "Dump Bilal UHS tables",
                .select = m6ii_bilal_dump,
                .icon_type = IT_ACTION,
                .help = "Dump the D8 equivalents of Bilal's uhs_regs and Canon presets.",
                .help2 = "Creates ML/LOGS/M6II_UHS.LOG. This action does not change SD state.",
            },
            MENU_EOL,
        },
    },
};

unsigned int init_SD_M6II(void)
{
    if (!is_camera("M6II", "1.1.1"))
        return 0;

    menu_add("Prefs", m6ii_bilal_menu, COUNT(m6ii_bilal_menu));
    DryosDebugMsg(0, 15, "M6II: Bilal sd_uhs port diagnostics loaded");

    return 0;
}
