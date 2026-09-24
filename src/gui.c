/**
 * MagicLantern GuiMainTask override
 * This was previously camera-specific
 **/

/*
 * Copyright (C) 2009 Trammell Hudson <hudson+ml@osresearch.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <gui.h>

#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <config-defines.h>
#include <boot-hack.h>
#include <menu.h>

/**
 * Supported cameras [E] Means it's enabled
 * [E] 1100D: counter_0x0c <-> msg_queue_0x30
 * [E] 600D : counter_0x0c <-> msg_queue_0x30
 * [E] 60D  : counter_0x0c <-> msg_queue_0x30
 * [E] 650D : counter_0x0c <-> msg_queue_0x30
 * [E] EOSM : counter_0x0c <-> msg_queue_0x30
 * [E] 700D : counter_0x0c <-> msg_queue_0x30
 * [E] 100D : counter_0x0c <-> msg_queue_0x30
 * [E] 5D3  : counter_0x0c <-> msg_queue_0x30
 * [E] 6D   : counter_0x0c <-> msg_queue_0x30
 */

/**
 * Easy to support cameras
 * [E] 550D : counter_0x04 <-> msg_queue_0x38
 * [D] 7D   : counter_0x04 <-> msg_queue_0x38
 */

/**
 * Unsupported cameras for now
 * 5D2  : counter_0x04 <-> msg_queue_0x34
 * 50D  : counter_0x04 <-> msg_queue_0x34
 * 500D : counter_0x04 <-> msg_queue_0x34
 */

struct semaphore * gui_sem;

#ifdef CONFIG_M6II
extern int movie_custom_updown_iso;
extern int movie_custom_set_x10;
extern int is_round_iso(int iso);
extern int GUI_GetLvDispSizeValue(void);
extern void GUI_SetLvDispSizeValue(int zoom);

void m6ii_movie_iso_step(int sign)
{
    /* Full-stop movie ISO sequence: 100 through 6400. */
    static const uint8_t m6ii_movie_iso_raw[] = {
        72, 80, 88, 96, 104, 112, 120
    };

    int current_raw = lens_info.raw_iso;

    if (!current_raw)
        return; /* leave Auto ISO untouched */

    if (sign > 0)
    {
        for (unsigned i = 0; i < COUNT(m6ii_movie_iso_raw); i++)
        {
            if (m6ii_movie_iso_raw[i] > current_raw)
            {
                m6ii_lens_set_rawiso_native(m6ii_movie_iso_raw[i]);
                return;
            }
        }
    }
    else
    {
        for (int i = COUNT(m6ii_movie_iso_raw) - 1; i >= 0; i--)
        {
            if (m6ii_movie_iso_raw[i] < current_raw)
            {
                m6ii_lens_set_rawiso_native(m6ii_movie_iso_raw[i]);
                return;
            }
        }
    }
}

static int handle_m6ii_movie_buttons_direct(struct event *event)
{
    /* M6 II movie shortcuts: UP raises ISO; SET toggles 1x/10x zoom. */
    if (!lv || gui_menu_shown())
        return 1;

    if (movie_custom_updown_iso)
    {
        if (event->param == 0x3F)
        {
            m6ii_movie_iso_step(+1);
            return 0;
        }

        if (event->param == 0x40)
            return 0;

    }

    if (movie_custom_set_x10)
    {
        if (event->param == 0x0A)
        {
            int zoom = GUI_GetLvDispSizeValue();
            GUI_SetLvDispSizeValue(zoom == 10 ? 1 : 10);
            return 0;
        }

        if (event->param == 0x0B)
            return 0;
    }

    return 1;
}
#endif

// return 0 if you want to block this event
static int handle_buttons(struct event * event)
{
    ASSERT(event->type == 0)

    if (event->type != 0) return 1; // only handle events with type=0 (buttons)

#ifdef CONFIG_M6II
    if (handle_m6ii_movie_buttons_direct(event) == 0)
        return 0;
#endif

    if (handle_common_events_startup(event) == 0) return 0;
    extern int ml_started;
    if (!ml_started) return 1;

// SJE Want to log button presses?  Uncomment the following line
//    DryosDebugMsg(0,15,"event: %08x", event->param);
    if (handle_common_events_by_feature(event) == 0) return 0;

    return 1;
}

/** TODO: rename stuff in this structure, don't use camera names maybe?
 * Other possibility, move in-structure offset to platform dir and define
 * per camera. */
struct gui_main_struct {
  void *          obj;                     // off_0x00;
  uint32_t        counter_550d;
  uint32_t        off_0x08;
  uint32_t        counter;                 // off_0x0c;
  uint32_t        off_0x10;
  uint32_t        off_0x14;
  struct msg_queue *    msg_queue_m50;     // off_0x18;
  struct msg_queue *    msg_queue_eosr;    // off_0x1C;
  uint32_t        off_0x20;
  uint32_t        off_0x24;
  uint32_t        off_0x28;
  uint32_t        off_0x2c;
  struct msg_queue *    msg_queue;         // off_0x30;
  struct msg_queue *    off_0x34;          // off_0x34;
  struct msg_queue *    msg_queue_550d;    // off_0x38;
  uint32_t        off_0x3c;
};

extern struct gui_main_struct gui_main_struct;

void ml_gui_main_task()
{
    struct event * event = NULL;
    int index = 0;
    void* funcs[GMT_NFUNCS];
    memcpy(funcs, (void*)GMT_FUNCTABLE, 4*GMT_NFUNCS);

    gui_init_end(); // no params?

#ifdef CONFIG_M6II
    uint32_t m6ii_gui_start_ms = get_ms_clock();
#endif

    while(1)
    {
        #if defined(CONFIG_550D) || defined(CONFIG_7D)
        msg_queue_receive(gui_main_struct.msg_queue_550d, &event, 0);
        gui_main_struct.counter_550d--;
        #elif defined(CONFIG_R) || defined(CONFIG_RP) || defined(CONFIG_850D) || defined (CONFIG_R5) || defined(CONFIG_SX70) || defined(CONFIG_M6II)
        msg_queue_receive(gui_main_struct.msg_queue_eosr, &event, 0);
        gui_main_struct.counter_550d--;
        #elif defined(CONFIG_M50) || defined(CONFIG_SX740)
        msg_queue_receive(gui_main_struct.msg_queue_m50, &event, 0);
        gui_main_struct.counter_550d--;
        #else
        msg_queue_receive(gui_main_struct.msg_queue, &event, 0);
        gui_main_struct.counter--;
        #endif

        if (event == NULL) {
            continue;
        }

#ifdef CONFIG_M6II
        /* Keep the startup SET bypass ahead of custom movie shortcuts. */
        if (event->type == 0 &&
            (event->param == BGMT_PRESS_SET ||
             event->param == BGMT_UNPRESS_SET) &&
            event->arg != FAKE_BTN &&
            get_ms_clock() - m6ii_gui_start_ms < 5000)
        {
            _disable_ml_startup();
            continue;
        }
#endif

        index = event->type;

        if (!magic_is_off())
        {

            if (event->type == 0)
            {
                if (handle_buttons(event) == 0) { // ML button/event handler
                    continue;
                }
            }
            else
            {
                if (handle_other_events(event) == 0) {
                    continue;
                }
            }
        }

        if (IS_FAKE(event)) {
           event->arg = 0;      /* do not pass the "fake" flag to Canon code */
        }

        if (event->type == 0 && event->param < 0) {
            continue;           /* do not pass internal ML events to Canon code */
        }

        if ((index >= GMT_NFUNCS) || (index < 0)) {
            continue;
        }

        void(*f)(struct event *) = funcs[index];
        if (f != NULL)
            f(event);
    }
}

TASK_OVERRIDE( gui_main_task, ml_gui_main_task);
