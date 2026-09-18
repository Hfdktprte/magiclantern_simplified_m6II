#include "sd_m6ii.h"

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <fio-ml.h>

extern int M6II_GetUHS2CardCapability(uint32_t dev, uint32_t *cap);
extern int M6II_IsUhs2Mode(uint32_t drive_letter, uint32_t *is_uhs2);
extern int M6II_SdChangeClockSpeed(uint32_t sd_handle, uint32_t clock_selector);
extern int M6II_SdCARDGetSpeed(uint32_t sd_handle, uint32_t *speed, uint32_t *clock_selector);

/*
 * M6II ROM 1.1.1 uses an SD device index as argument 0 for these APIs.
 * DebugSTG_IsUHSCard explicitly calls the same driver family with r0 = 0.
 * The DebugSTG state at 0xE384 also carries device 0 at +0x14; zero is
 * therefore a valid device ID, not a NULL handle.
 */
#define M6II_SD_DEVICE 0

struct m6ii_sd_device
{
    void *read_block;
    void *write_block;
    void *io_control;
    void *soft_reset;
};

extern struct m6ii_sd_device * const sd_device[];

static volatile int m6ii_sd_test_busy = 0;


static int m6ii_sd_get_speed_clock(uint32_t *speed, uint32_t *clock)
{
    return M6II_SdCARDGetSpeed(M6II_SD_DEVICE, speed, clock);
}

static void m6ii_sd_speed_task(void *unused)
{
    uint32_t speed = 0xffffffff;
    uint32_t clock = 0xffffffff;
    int err = m6ii_sd_get_speed_clock(&speed, &clock);

    DryosDebugMsg(0, 15,
                  "M6II SD: dev=%d speed=%d clock=%d err=%d",
                  M6II_SD_DEVICE, speed, clock, err);
    NotifyBox(7000, "SDR speed=%d clock=%d\ndev=%d err=%d",
              speed, clock, M6II_SD_DEVICE, err);

    m6ii_sd_test_busy = 0;
}

static void m6ii_sd_clock_task(void *arg)
{
    uint32_t wanted = (uint32_t)(uintptr_t)arg;
    uint32_t before_speed = 0xffffffff;
    uint32_t before_clock = 0xffffffff;
    uint32_t after_speed = 0xffffffff;
    uint32_t after_clock = 0xffffffff;
    int before_err = M6II_SdCARDGetSpeed(M6II_SD_DEVICE, &before_speed, &before_clock);
    if (before_err)
    {
        NotifyBox(5000, "GetSpeed before failed: %d", before_err);
        m6ii_sd_test_busy = 0;
        return;
    }

    int set_err = M6II_SdChangeClockSpeed(M6II_SD_DEVICE, wanted);
    msleep(50);
    int after_err = M6II_SdCARDGetSpeed(M6II_SD_DEVICE, &after_speed, &after_clock);

    DryosDebugMsg(0, 15,
                  "M6II SD clock: %d/%d -> sel %d ret=%d -> %d/%d get=%d",
                  before_speed, before_clock, wanted, set_err,
                  after_speed, after_clock, after_err);

    NotifyBox(9000,
              "SD clock %d -> %d\nset=%d get=%d speed=%d",
              before_clock, after_clock, set_err, after_err, after_speed);

    m6ii_sd_test_busy = 0;
}

static MENU_SELECT_FUNC(m6ii_sd_read_speed)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_speed", 0x1c, 0x1000, m6ii_sd_speed_task, 0);
}

static void m6ii_sd_start_clock(uint32_t selector)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_clock", 0x1c, 0x1000,
                m6ii_sd_clock_task, (void *)(uintptr_t)selector);
}

static MENU_SELECT_FUNC(m6ii_sd_clock_156)
{
    m6ii_sd_start_clock(8);
}

static MENU_SELECT_FUNC(m6ii_sd_clock_stock)
{
    m6ii_sd_start_clock(9);
}


static void m6ii_sd_uhs1_task(void *unused)
{
    int is_uhs = call("DebugSTG_IsUHSCard");
    int is_uhs2 = call("DebugSTG_IsUHS2Card");

    DryosDebugMsg(0, 15, "M6II SD: IsUHSCard=%d IsUHS2Card=%d", is_uhs, is_uhs2);

    if (is_uhs && !is_uhs2)
        NotifyBox(6000, "SD interface: UHS-I\nUHS=%d UHS-II=%d", is_uhs, is_uhs2);
    else if (is_uhs2)
        NotifyBox(6000, "SD interface: UHS-II\nUHS=%d UHS-II=%d", is_uhs, is_uhs2);
    else
        NotifyBox(6000, "SD interface: non-UHS\nUHS=%d UHS-II=%d", is_uhs, is_uhs2);

    m6ii_sd_test_busy = 0;
}

static void m6ii_sd_dump_task(void *unused)
{
    int is_uhs = call("DebugSTG_IsUHSCard");
    int is_uhs2 = call("DebugSTG_IsUHS2Card");
    uint32_t mounted_uhs2 = 0xffffffff;
    int mounted_uhs2_err;

    uint32_t old_int = cli();
    mounted_uhs2_err = M6II_IsUhs2Mode('B', &mounted_uhs2);
    sei(old_int);

    /*
     * Ask Canon to print its full parsed SD-card information to DryOS debug.
     * This is read-only; the return value is useful even when debug output
     * itself is not being captured.
     */
    int card_info_ret = call("DebugSTG_GetSDCardInfo");
    msleep(100);

    /*
     * dumpf writes Canon/ML's internal DryosDebugMsg ring to logNNNN.log.
     * The GetSDCardInfo output includes the SD-UHS1 speed/clock and
     * supported access-mode/driver-strength lines we are reverse engineering.
     */
    call("dumpf");

    FILE *f = FIO_CreateFile("ML/LOGS/M6II_SD.LOG");
    if (!f)
    {
        NotifyBox(5000, "Could not create M6II_SD.LOG");
        m6ii_sd_test_busy = 0;
        return;
    }

    my_fprintf(f, "M6II 1.1.1 SD diagnostic\n");
    my_fprintf(f, "DebugSTG_IsUHSCard=%d\n", is_uhs);
    my_fprintf(f, "DebugSTG_IsUHS2Card=%d\n", is_uhs2);
    my_fprintf(f, "M6II_IsUhs2Mode(B): err=%d value=%#x\n",
               mounted_uhs2_err, mounted_uhs2);
    my_fprintf(f, "DebugSTG_GetSDCardInfo ret=%d\n", card_info_ret);
    my_fprintf(f, "Canon DryOS debug ring dumped with dumpf (logNNNN.log)\n");

    /*
     * M6II follows the generic single-SD-slot bootflags path and uses
     * sd_device[1].  Only read the four established device fields.
     */
    struct m6ii_sd_device *dev = sd_device[1];
    my_fprintf(f, "sd_device table=%08x\n", (uint32_t)sd_device);
    my_fprintf(f, "sd_device[1]=%08x\n", (uint32_t)dev);
    if (dev)
    {
        my_fprintf(f, "read_block=%08x\n", (uint32_t)dev->read_block);
        my_fprintf(f, "write_block=%08x\n", (uint32_t)dev->write_block);
        my_fprintf(f, "io_control=%08x\n", (uint32_t)dev->io_control);
        my_fprintf(f, "soft_reset=%08x\n", (uint32_t)dev->soft_reset);

        uint32_t *w = (uint32_t *)dev;
        for (int i = 0; i < 16; i++)
            my_fprintf(f, "dev[%02d]=%08x\n", i, w[i]);
    }

    FIO_CloseFile(f);

    DryosDebugMsg(0, 15, "M6II SD: diagnostic saved to ML/LOGS/M6II_SD.LOG");
    NotifyBox(7000, "Saved M6II_SD.LOG + Canon debug log");

    m6ii_sd_test_busy = 0;
}

static MENU_SELECT_FUNC(m6ii_sd_read_uhs1)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_uhs1", 0x1c, 0x1000, m6ii_sd_uhs1_task, 0);
}

static MENU_SELECT_FUNC(m6ii_sd_dump_state)
{
    if (m6ii_sd_test_busy)
    {
        NotifyBox(2000, "SD test already running");
        return;
    }

    m6ii_sd_test_busy = 1;
    task_create("m6ii_sd_dump", 0x1c, 0x1800, m6ii_sd_dump_task, 0);
}

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
                .name = "Read UHS-I / UHS-II",
                .select = m6ii_sd_read_uhs1,
                .icon_type = IT_ACTION,
                .help = "Ask Canon whether the mounted card is UHS and/or UHS-II.",
                .help2 = "Read-only. UHS=1 and UHS-II=0 identifies the UHS-I path.",
            },
            {
                .name = "Read current UHS-II mode",
                .select = m6ii_sd_read_mode,
                .icon_type = IT_ACTION,
                .help = "Query Canon's mounted-card UHS-II state for B:/.",
                .help2 = "Read-only. Mirrors DebugSTG_CheckUHS2Mode from M6II 1.1.1 ROM.",
            },
            {
                .name = "Read speed / clock",
                .select = m6ii_sd_read_speed,
                .icon_type = IT_ACTION,
                .help = "Call Canon SdCARDGetSpeed for SD device 0.",
                .help2 = "Read-only. Stock SDR104 is speed=5, clock=9 on M6II 1.1.1.",
            },
            {
                .name = "Test clock: 156 MHz",
                .select = m6ii_sd_clock_156,
                .icon_type = IT_ACTION,
                .help = "Change only Canon's UHS-I clock selector from 9 to 8.",
                .help2 = "Reversible validation step. Use Restore stock 195 MHz afterwards.",
            },
            {
                .name = "Restore stock: 195 MHz",
                .select = m6ii_sd_clock_stock,
                .icon_type = IT_ACTION,
                .help = "Restore Canon's stock SDR104 clock selector 9.",
                .help2 = "M6II ROM maps selector 9 to 195 MHz.",
            },
            {
                .name = "Save SD diagnostic",
                .select = m6ii_sd_dump_state,
                .icon_type = IT_ACTION,
                .help = "Save mounted SD/UHS state to ML/LOGS/M6II_SD.LOG.",
                .help2 = "Also dumps Canon's DryOS SD-card information trace.",
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
