#include "sd_m6ii.h"

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <fio-ml.h>
#include <patch.h>
#include <config.h>

extern int M6II_SdCARDGetSpeed(uint32_t dev, uint32_t *speed, uint32_t *clock_selector);
extern int M6II_SD_ReConfiguration(void);
extern int M6II_DebugSTG_IsUHSCard(void);

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


#define M6II_SELECTOR9_TABLE 0x0001E5E8

/* Known-good Canon device-0 blocks from the user's M6II table dump. */
static const uint32_t m6ii_canon_156[11] =
{
    0x00000001, 0x00000001, 0x1D000001, 0x00000000,
    0x00000100, 0x00000100, 0x00000100, 0x00000100,
    0x00000000, 0x00000001, 0x00000001,
};

static const uint32_t m6ii_canon_195[11] =
{
    0x00000001, 0x00000001, 0x1D000001, 0x00000000,
    0x00000100, 0x00000100, 0x00000100, 0x00000100,
    0x00000000, 0x00000000, 0x00000001,
};

static void m6ii_copy_words(volatile uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = src[i];
}

static int m6ii_regs_match(const uint32_t *vals)
{
    for (int i = 0; i < COUNT(m6ii_uhs_regs); i++)
    {
        if (MEM(m6ii_uhs_regs[i]) != vals[i])
            return 0;
    }
    return 1;
}

/*
 * Bilal/a1ex porting mechanism:
 *  - hook immediately after Canon programs the SD controller registers,
 *  - override them with uhs_vals[],
 *  - call the no-argument SD_ReConfiguration() while card I/O is quiescent.
 *
 * On M6II, every valid clock selector converges here immediately after
 * 0xE012BA6C writes the 11-word D01006xx controller preset.
 */
#define M6II_SD_POST_PRESET_HOOK 0xE012BEA4

static CONFIG_INT("sd.m6ii_bilal_boot_test", m6ii_bilal_boot_test, 0);

static uint32_t m6ii_uhs_vals[11];
static struct patch m6ii_bilal_hook_patch[1];
static uint8_t m6ii_bilal_hook_code[8] __attribute__((aligned(4)));
static int m6ii_bilal_hook_installed = 0;

static volatile uint32_t m6ii_hook_calls = 0;
static volatile uint32_t m6ii_hook_hits = 0;
static volatile uint32_t m6ii_hook_last_dev = 0xffffffff;
static volatile uint32_t m6ii_hook_last_selector = 0xffffffff;

static int m6ii_boot_test_ran = 0;
static int m6ii_boot_patch_rc = -1;
static int m6ii_boot_reconfig_rc = -1;
static int m6ii_boot_get_before = -1;
static int m6ii_boot_get_after = -1;
static int m6ii_boot_uhs_before = -1;
static int m6ii_boot_uhs_after = -1;
static uint32_t m6ii_boot_speed_before = 0xffffffff;
static uint32_t m6ii_boot_clock_before = 0xffffffff;
static uint32_t m6ii_boot_speed_after = 0xffffffff;
static uint32_t m6ii_boot_clock_after = 0xffffffff;
static int m6ii_boot_live_195 = 0;
static uint32_t m6ii_boot_div = 0xffffffff;

static void m6ii_bilal_post_preset_override(uint32_t dev, uint32_t selector)
{
    m6ii_hook_calls++;
    m6ii_hook_last_dev = dev;
    m6ii_hook_last_selector = selector;

    if (dev == M6II_SD_DEVICE && selector == 9)
    {
        /*
         * Stock validation uses Canon's own 195-MHz values.
         * Later, Bilal-style 160/192/240 D8 arrays will occupy this same
         * uhs_vals[] buffer; the hook mechanism itself will not change.
         */
        for (int i = 0; i < COUNT(m6ii_uhs_regs); i++)
            MEM(m6ii_uhs_regs[i]) = m6ii_uhs_vals[i];

        m6ii_hook_hits++;
    }
}

/*
 * D678X convert_f_patch_to_patch() replaces 8 bytes with an absolute jump;
 * unlike the old D5 patch_hook_function(), it does not manufacture a
 * callback frame.  Use the same proven pattern as the M6II RAW EDMAC hook:
 * preserve Canon's registers, call our helper, replay the displaced
 * instructions semantically, then resume after the 8-byte patch.
 *
 * Displaced E012BEA4..E012BEAB:
 *   movs r1,#5
 *   mov  r0,r5
 *   bl   E043DC08
 */
static void __attribute__((noinline,naked,aligned(4)))
m6ii_bilal_post_preset_trampoline(void)
{
    asm volatile(
        "push {r0-r12, lr}\n"
        "mov  r0, r5\n"
        "mov  r1, r4\n"
        "mov  r3, %0\n"
        "blx  r3\n"
        "pop  {r0-r12, lr}\n"

        /* replay displaced instructions */
        "movs r1, #5\n"
        "mov  r0, r5\n"
        "movw r3, #0xDC09\n"
        "movt r3, #0xE043\n"
        "blx  r3\n"

        /* resume at E012BEAC */
        "movw r3, #0xBEAD\n"
        "movt r3, #0xE012\n"
        "bx   r3\n"
        :
        : "r"(m6ii_bilal_post_preset_override)
        : "r3"
    );
}

static int m6ii_install_bilal_hook(void)
{
    struct function_hook_patch def =
    {
        .patch_addr = M6II_SD_POST_PRESET_HOOK,
        .target_function_addr = (uint32_t)m6ii_bilal_post_preset_trampoline,
        .description = "sd_clock: M6II Bilal post-preset override",
    };

    static const uint8_t expected[8] =
    {
        0x05, 0x21, 0x28, 0x46, 0x11, 0xF3, 0xAE, 0xFE
    };

    for (int i = 0; i < 8; i++)
        def.orig_content[i] = expected[i];

    int err = convert_f_patch_to_patch(&def,
                                      &m6ii_bilal_hook_patch[0],
                                      &m6ii_bilal_hook_code[0]);
    if (err != E_PATCH_OK)
        return err;

    err = apply_patches(m6ii_bilal_hook_patch, 1);
    if (err == E_PATCH_OK)
        m6ii_bilal_hook_installed = 1;

    return err;
}

static void m6ii_remove_bilal_hook(void)
{
    if (!m6ii_bilal_hook_installed)
        return;

    unpatch_memory(M6II_SD_POST_PRESET_HOOK);
    m6ii_bilal_hook_installed = 0;
}

static void m6ii_run_startup_stock_validation(void)
{
    m6ii_boot_test_ran = 1;
    m6ii_hook_calls = 0;
    m6ii_hook_hits = 0;
    m6ii_hook_last_dev = 0xffffffff;
    m6ii_hook_last_selector = 0xffffffff;

    m6ii_copy_words(m6ii_uhs_vals, m6ii_canon_195, 11);

    m6ii_boot_get_before =
        M6II_SdCARDGetSpeed(M6II_SD_DEVICE,
                            &m6ii_boot_speed_before,
                            &m6ii_boot_clock_before);
    m6ii_boot_uhs_before = M6II_DebugSTG_IsUHSCard();

    m6ii_boot_patch_rc = m6ii_install_bilal_hook();
    if (m6ii_boot_patch_rc == E_PATCH_OK)
    {
        /*
         * This is intentionally executed from module startup, matching
         * Bilal's sd_uhs_init -> sd_overclock_task flow.  Do not move this
         * back into a live menu action: SD_ReConfiguration is not thread-safe.
         */
        m6ii_boot_reconfig_rc = M6II_SD_ReConfiguration();

        m6ii_boot_get_after =
            M6II_SdCARDGetSpeed(M6II_SD_DEVICE,
                                &m6ii_boot_speed_after,
                                &m6ii_boot_clock_after);
        m6ii_boot_uhs_after = M6II_DebugSTG_IsUHSCard();
        m6ii_boot_live_195 = m6ii_regs_match(m6ii_canon_195);
        m6ii_boot_div = MEM(0xD0100604);

        m6ii_remove_bilal_hook();
    }

    /*
     * One-shot in RAM.  After a successful boot/shutdown, config persistence
     * should leave this disabled for the next startup.
     */
    m6ii_bilal_boot_test = 0;

    DryosDebugMsg(0, 15,
                  "M6II Bilal boot stock: patch=%d rc=%d get=%d/%d %d/%d->%d/%d UHS=%d/%d calls=%d hits=%d last=%d/%d live195=%d div=%d",
                  m6ii_boot_patch_rc, m6ii_boot_reconfig_rc,
                  m6ii_boot_get_before, m6ii_boot_get_after,
                  m6ii_boot_speed_before, m6ii_boot_clock_before,
                  m6ii_boot_speed_after, m6ii_boot_clock_after,
                  m6ii_boot_uhs_before, m6ii_boot_uhs_after,
                  m6ii_hook_calls, m6ii_hook_hits,
                  m6ii_hook_last_dev, m6ii_hook_last_selector,
                  m6ii_boot_live_195, m6ii_boot_div);
}

static MENU_SELECT_FUNC(m6ii_show_boot_result)
{
    if (!m6ii_boot_test_ran)
    {
        NotifyBox(7000,
                  "No startup validation this boot.\nSet Boot stock validation=ON, then restart.");
        return;
    }

    NotifyBox(15000,
              "Bilal boot stock\npatch=%d rc=%d get=%d/%d\nlogical %d/%d -> %d/%d\nUHS=%d->%d calls=%d hits=%d\nlast=%d/%d live195=%d div=%d",
              m6ii_boot_patch_rc, m6ii_boot_reconfig_rc,
              m6ii_boot_get_before, m6ii_boot_get_after,
              m6ii_boot_speed_before, m6ii_boot_clock_before,
              m6ii_boot_speed_after, m6ii_boot_clock_after,
              m6ii_boot_uhs_before, m6ii_boot_uhs_after,
              m6ii_hook_calls, m6ii_hook_hits,
              m6ii_hook_last_dev, m6ii_hook_last_selector,
              m6ii_boot_live_195, m6ii_boot_div);
}

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
    my_fprintf(f, "SD_ReConfiguration=E00BA8CC (no args)\n");
    my_fprintf(f, "D8 preset switch=E012BB44 writer=E012BA6C\n");
    my_fprintf(f, "Bilal post-preset hook=E012BEA4\n");

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
                .name = "Boot stock validation",
                .priv = &m6ii_bilal_boot_test,
                .max = 1,
                .choices = CHOICES("OFF", "ON next restart"),
                .help = "Bilal-style one-shot stock validation during module startup.",
                .help2 = "Set ON, then restart. No overclock values; uses Canon's 195MHz array.",
            },
            {
                .name = "Show startup result",
                .select = m6ii_show_boot_result,
                .icon_type = IT_ACTION,
                .help = "Show the result captured by the Bilal startup validation.",
                .help2 = "Includes hook calls/hits, UHS state, logical speed/clock and live preset match.",
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

    if (m6ii_bilal_boot_test)
        m6ii_run_startup_stock_validation();

    DryosDebugMsg(0, 15, "M6II: Bilal sd_uhs port diagnostics loaded");

    return 0;
}
