#include "dryos.h"
#include "bmp.h"
#include "tasks.h"
#include "debug.h"
#include "math.h"
#include "menu.h"
#include "property.h"
#include "config.h"
#include "gui.h"
#include "lens.h"
#include "version.h"
#include "edmac.h"
#include "asm.h"
#include "lvinfo.h"

#ifdef CONFIG_ELECTRONIC_LEVEL

/* Canon stub */
extern void GUI_SetRollingPitchingLevelStatus(int request);
#ifdef CONFIG_M6II
extern int GUI_GetRollingPitchingLevelStatus(void);
extern int GUI_GetRollingPitchingLevelRoll(void);
extern int GUI_GetRollingPitchingLevelPitch(void);
#endif

struct rolling_pitching
{
    uint8_t status;
    uint8_t cameraposture;
    uint8_t roll_sensor1;
    uint8_t roll_sensor2;
    uint8_t pitch_sensor1;
    uint8_t pitch_sensor2;
};

static struct rolling_pitching level_data;

CONFIG_INT("electronic.level.mode", electronic_level_mode, 2);
CONFIG_INT("electronic.level.thickness", electronic_level_thickness, 0);

static int electronic_level_status(void)
{
#ifdef CONFIG_M6II
    return GUI_GetRollingPitchingLevelStatus();
#else
    return level_data.status;
#endif
}

static int electronic_level_roll(void)
{
#ifdef CONFIG_M6II
    return GUI_GetRollingPitchingLevelRoll();
#else
    return level_data.roll_sensor1 * 256 + level_data.roll_sensor2;
#endif
}

static int electronic_level_pitch(void)
{
#ifdef CONFIG_M6II
    return GUI_GetRollingPitchingLevelPitch();
#else
    return level_data.pitch_sensor1 * 256 + level_data.pitch_sensor2;
#endif
}

PROP_HANDLER(PROP_ROLLING_PITCHING_LEVEL)
{
    memcpy(&level_data, buf, 6);
}

static void draw_level_line(int x1, int y1, int x2, int y2, int dx, int dy, int color1, int color2, int thickness)
{
    if (thickness)
        draw_line(x1 - dx, y1 - dy, x2 - dx, y2 - dy, color1);
    draw_line(x1, y1, x2, y2, color1);
    draw_line(x1 + dx, y1 + dy, x2 + dx, y2 + dy, color2);
    if (thickness)
        draw_line(x1 + 2 * dx, y1 + 2 * dy, x2 + 2 * dx, y2 + 2 * dy, color2);
}

static void draw_level_lines(int angle, int pitch, int show, int mode, int thickness)
{
    int x0 = os.x0 + os.x_ex/2;
    int y0 = os.y0 + os.y_ex/2;
    int r = 100;

    #define MUL 16384
    #define PI_1800 0.00174532925

    int s = sinf(angle * PI_1800) * MUL;
    int c = cosf(angle * PI_1800) * MUL;
    int x_offset = r * c / MUL;
    int y_offset = r * s / MUL;
    int dx = (angle % 1800 >= 450 && angle % 1800 < 1350);
    int dy = 1 - dx;

    int color1 = show ? ((angle % 900) ? COLOR_BLACK : COLOR_GREEN1) : 0;
    int color2 = show ? ((angle % 900) ? COLOR_WHITE : COLOR_GREEN2) : 0;

    if (mode != 0)
        draw_level_line(x0 - x_offset, y0 - y_offset, x0 + x_offset, y0 + y_offset, dx, dy, color1, color2, thickness);

    static int pitch_changed = 0;
#ifdef CONFIG_M6II
    pitch_changed = 1;
#else
    if (pitch != 0) pitch_changed = 1;
#endif
    if (!pitch_changed || mode == 1) return;

    int pitch_s = sinf(pitch * PI_1800) * MUL;

    r = 120;
    x0 -= (r * pitch_s / MUL) * s / MUL;
    y0 += (r * pitch_s / MUL) * c / MUL;
    x_offset /= 2;
    y_offset /= 2;

    color1 = show ? ((pitch % 900) ? COLOR_BLACK : COLOR_GREEN1) : 0;
    color2 = show ? ((pitch % 900) ? COLOR_WHITE : COLOR_GREEN2) : 0;

    draw_level_line(x0 - x_offset, y0 - y_offset, x0 + x_offset, y0 + y_offset, dx, dy, color1, color2, thickness);
}
static void draw_electronic_level(int angle, int prev_angle, int pitch, int prev_pitch, int force_redraw)
{
    static int prev_mode = 2;
    static int prev_thickness = 0;
    int mode = electronic_level_mode;
    int thickness = electronic_level_thickness;

    if (!force_redraw && angle == prev_angle && pitch == prev_pitch &&
        mode == prev_mode && thickness == prev_thickness) return;

    draw_level_lines(prev_angle, prev_pitch, 0, prev_mode, prev_thickness);
    draw_level_lines(angle, pitch, 1, mode, thickness);
    prev_mode = mode;
    prev_thickness = thickness;
#ifdef FEATURE_VRAM_RGBA
    ml_refresh_display_needed = 1;
#endif
}

void disable_electronic_level()
{
    if (electronic_level_status() == 2)
    {
        GUI_SetRollingPitchingLevelStatus(1);
        msleep(100);
    }
}

void show_electronic_level()
{
    static int prev_angle10 = 0;
    static int prev_pitch10 = 0;
    int force_redraw = 0;
    static int k = 0;
    k++;

#ifdef CONFIG_M6II
    if (electronic_level_status() != 2 || k % 10 == 0)
    {
        GUI_SetRollingPitchingLevelStatus(0);
        if (electronic_level_status() != 2) msleep(100);
        force_redraw = 1;
    }
#else
    if (electronic_level_status() != 2)
    {
        GUI_SetRollingPitchingLevelStatus(0);
        msleep(100);
        force_redraw = 1;
    }
    if (k % 10 == 0) force_redraw = 1;
#endif

    int angle10 = electronic_level_roll() / 10;
    int pitch10 = electronic_level_pitch() / 10;
    draw_electronic_level(angle10, prev_angle10, pitch10, prev_pitch10, force_redraw);
    prev_angle10 = angle10;
    prev_pitch10 = pitch10;
}


#endif
