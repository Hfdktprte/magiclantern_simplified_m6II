/** \file
 * M6II.111 read-only exposure-state snapshot.
 *
 * Dumps the LiveView AE state and lens_info without changing camera settings.
 * Take snapshots at two or more deliberately different manual exposures; the
 * changing fields identify Canon's authoritative Tv/Av/ISO readback.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>
#include <lens.h>

#define M6II_LVAE_STRUCT      0x0005167Cu
#define M6II_LVAE_ISO_STRUCT  0x00069FE4u
#define M6II_EXPO_DUMP_BYTES  0x100u

static unsigned int m6ii_expo_seq = 0;

static inline uint16_t m6ii_read16(uint32_t addr)
{
    return *(volatile uint16_t *)addr;
}

static inline uint8_t m6ii_read8(uint32_t addr)
{
    return *(volatile uint8_t *)addr;
}

static void m6ii_write_u16_dump(FILE *f, const char *name, uint32_t base)
{
    char line[160];

    int n = snprintf(line, sizeof(line), "\n[%s] base=0x%08x bytes=0x%x\n",
                     name, base, M6II_EXPO_DUMP_BYTES);
    FIO_WriteFile(f, line, n);

    for (uint32_t off = 0; off < M6II_EXPO_DUMP_BYTES; off += 2)
    {
        uint16_t v = m6ii_read16(base + off);
        n = snprintf(line, sizeof(line), "+%02x = %04x\n", off, v);
        FIO_WriteFile(f, line, n);
    }
}

static void m6ii_exposure_snapshot(void)
{
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "Exposure snapshot requires LiveView");
        return;
    }

    char filename[40];
    unsigned int seq = m6ii_expo_seq++;
    snprintf(filename, sizeof(filename), "M6II_EXPO_%02u.LOG", seq);

    /* Snapshot volatile values before file I/O. */
    uint16_t control_bv   = m6ii_read16(M6II_LVAE_STRUCT + 0x28);
    uint16_t control_tv   = m6ii_read16(M6II_LVAE_STRUCT + 0x3E);
    uint16_t control_av   = m6ii_read16(M6II_LVAE_STRUCT + 0x40);
    uint16_t control_iso  = m6ii_read16(M6II_LVAE_STRUCT + 0x42);
    uint16_t control_zero = m6ii_read16(M6II_LVAE_STRUCT + 0x44);
    uint16_t disp_gain    = m6ii_read16(M6II_LVAE_STRUCT + 0x70);
    uint8_t  mov_m_ctrl   = m6ii_read8 (M6II_LVAE_STRUCT + 0x24);

    unsigned raw_shutter  = lens_info.raw_shutter;
    unsigned raw_aperture = lens_info.raw_aperture;
    unsigned raw_iso      = lens_info.raw_iso;
    unsigned raw_iso_auto = lens_info.raw_iso_auto;
    unsigned shutter      = lens_info.shutter;
    unsigned aperture     = lens_info.aperture;
    unsigned iso          = lens_info.iso;
    unsigned iso_auto     = lens_info.iso_auto;

    FILE *f = FIO_CreateFile(filename);
    if (!f)
    {
        NotifyBox(3000, "Could not create %s", filename);
        return;
    }

    char line[512];
    int n = snprintf(line, sizeof(line),
        "M6II exposure snapshot v1\n"
        "sequence=%u\n"
        "lvae_base=0x%08x\n"
        "iso_base=0x%08x\n"
        "CONTROL_BV_28=0x%04x\n"
        "CONTROL_TV_3e=0x%04x\n"
        "CONTROL_AV_40=0x%04x\n"
        "CONTROL_ISO_42=0x%04x\n"
        "CONTROL_ZERO_44=0x%04x\n"
        "DISP_GAIN_70=0x%04x\n"
        "MOV_M_CTRL_24=0x%02x\n"
        "lens_raw_shutter=%u (0x%02x)\n"
        "lens_raw_aperture=%u (0x%02x)\n"
        "lens_raw_iso=%u (0x%02x)\n"
        "lens_raw_iso_auto=%u (0x%02x)\n"
        "lens_shutter=%u\n"
        "lens_aperture=%u\n"
        "lens_iso=%u\n"
        "lens_iso_auto=%u\n",
        seq,
        M6II_LVAE_STRUCT,
        M6II_LVAE_ISO_STRUCT,
        control_bv,
        control_tv,
        control_av,
        control_iso,
        control_zero,
        disp_gain,
        mov_m_ctrl,
        raw_shutter, raw_shutter,
        raw_aperture, raw_aperture,
        raw_iso, raw_iso,
        raw_iso_auto, raw_iso_auto,
        shutter,
        aperture,
        iso,
        iso_auto);
    FIO_WriteFile(f, line, n);

    m6ii_write_u16_dump(f, "LVAE", M6II_LVAE_STRUCT);
    m6ii_write_u16_dump(f, "LVAE_ISO", M6II_LVAE_ISO_STRUCT);

    FIO_CloseFile(f);

    NotifyBox(6000,
        "%s\nCBV=%04x TV=%04x AV=%04x ISO=%04x\nlens raw T/A/I=%02x/%02x/%02x",
        filename, control_bv, control_tv, control_av, control_iso,
        raw_shutter, raw_aperture, raw_iso);
}

static struct menu_entry m6ii_exposure_menu[] = {
    {
        .name   = "M6II exposure snapshot",
        .priv   = m6ii_exposure_snapshot,
        .select = run_in_separate_task,
        .help   = "Read-only LVAE/lens exposure snapshot. Run at two different manual exposures."
    },
};

static void m6ii_exposure_probe_init(void *unused)
{
    (void)unused;
    menu_add("Debug", m6ii_exposure_menu, COUNT(m6ii_exposure_menu));
}

INIT_FUNC(__FILE__, m6ii_exposure_probe_init);

#endif
