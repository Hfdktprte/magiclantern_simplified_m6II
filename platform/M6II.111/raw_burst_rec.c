/** \file
 * EOS M6 Mark II first RAW burst recorder.
 *
 * Experimental bring-up recorder: uses Canon's already-proven RAW LiveView
 * path and the ROM-derived LiveView state structure to snapshot the current
 * 14-bit packed RAW buffer. No EDMAC redirection, no guessed MMIO, no SRM.
 *
 * Output is a simple self-describing M6RB container, not MLV yet. The purpose
 * is to prove that consecutive real RAW frames can be captured and written.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>

#define M6II_RAW_STATE_BASE       0x00010970
#define M6II_BURST_FRAMES         8
#define M6II_BURST_INTERVAL_MS    120
#define M6II_BURST_FILE           "M6II_RAW_BURST.M6RB"
#define M6II_BURST_INFO           "M6II_RAW_BURST.TXT"

struct m6rb_file_header
{
    char magic[4];              /* M6RB */
    uint32_t version;
    uint32_t width_units;
    uint32_t height_lines;
    uint32_t frame_size;
    uint32_t frame_count;
    uint32_t interval_ms;
    uint32_t reserved;
} __attribute__((packed));

struct m6rb_frame_header
{
    char magic[4];              /* FRAM */
    uint32_t frame_index;
    uint32_t source_address;
    uint32_t frame_size;
    uint32_t timestamp_ms;
} __attribute__((packed));

static volatile int m6ii_burst_busy = 0;

static uint32_t state32(uint32_t off)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + off);
}

static int write_all(FILE *f, const void *buf, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < size)
    {
        uint32_t chunk = MIN(1024 * 1024, size - done);
        int n = FIO_WriteFile(f, p + done, chunk);
        if (n != (int)chunk)
            return 0;
        done += chunk;
    }
    return 1;
}

static void m6ii_raw_burst_record()
{
    if (m6ii_burst_busy)
    {
        NotifyBox(2000, "RAW burst already running");
        return;
    }

    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW burst requires active LiveView");
        return;
    }

    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_burst_busy = 1;
    DryosDebugMsg(0, 15, "M6II RAW burst: BEGIN");

    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);
    msleep(500);

    uint32_t w = state32(0x10);
    uint32_t h = state32(0x14);
    uint32_t raw_type = state32(0x44);
    uint32_t src = state32(0x58);

    uint32_t frame_size = 0;
    if (w && h && w < 0x10000 && h < 0x10000)
        frame_size = (w * h * 7) / 4;

    /* Refuse obviously bad state rather than dereferencing an invalid pointer. */
    if (!src || frame_size < 1024 * 1024 || frame_size > 80 * 1024 * 1024)
    {
        call("lv_save_raw", 0);
        m6ii_burst_busy = 0;
        NotifyBox(6000, "RAW burst refused: w=%u h=%u buf=%08x size=%u",
                  w, h, src, frame_size);
        return;
    }

    void *frame = malloc(frame_size);
    if (!frame)
    {
        call("lv_save_raw", 0);
        m6ii_burst_busy = 0;
        NotifyBox(5000, "RAW burst: malloc(%u) failed", frame_size);
        return;
    }

    FILE *f = FIO_CreateFile(M6II_BURST_FILE);
    if (!f)
    {
        free(frame);
        call("lv_save_raw", 0);
        m6ii_burst_busy = 0;
        NotifyBox(5000, "RAW burst: cannot create output file");
        return;
    }

    struct m6rb_file_header fh = {
        .magic = {'M','6','R','B'},
        .version = 1,
        .width_units = w,
        .height_lines = h,
        .frame_size = frame_size,
        .frame_count = M6II_BURST_FRAMES,
        .interval_ms = M6II_BURST_INTERVAL_MS,
        .reserved = raw_type,
    };

    int ok = write_all(f, &fh, sizeof(fh));
    uint32_t captured = 0;

    for (uint32_t i = 0; ok && i < M6II_BURST_FRAMES; i++)
    {
        /* Re-read Canon's pointer each frame in case it rotates buffers. */
        src = state32(0x58);
        if (!src)
        {
            ok = 0;
            break;
        }

        /* Snapshot first; file I/O happens from our private copy so Canon can
         * continue updating its RAW buffer while the card write is in flight. */
        memcpy(frame, (void *)src, frame_size);

        struct m6rb_frame_header fr = {
            .magic = {'F','R','A','M'},
            .frame_index = i,
            .source_address = src,
            .frame_size = frame_size,
            .timestamp_ms = get_ms_clock(),
        };

        ok = write_all(f, &fr, sizeof(fr));
        if (ok)
            ok = write_all(f, frame, frame_size);

        if (ok)
            captured++;

        NotifyBox(700, "RAW burst %u/%u", captured, M6II_BURST_FRAMES);
        msleep(M6II_BURST_INTERVAL_MS);
    }

    FIO_CloseFile(f);
    int ret_off = call("lv_save_raw", 0);
    free(frame);

    FILE *info = FIO_CreateFile(M6II_BURST_INFO);
    if (info)
    {
        char text[512];
        int len = snprintf(text, sizeof(text),
            "M6II RAW burst v1\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "lv_save_raw_0=0x%08x\n"
            "state_base=0x%08x\n"
            "width_units=%u\n"
            "height_lines=%u\n"
            "raw_type=0x%08x\n"
            "frame_size=%u\n"
            "requested_frames=%u\n"
            "captured_frames=%u\n"
            "interval_ms=%u\n"
            "write_ok=%u\n",
            ret_mm, ret_on, ret_off,
            M6II_RAW_STATE_BASE, w, h, raw_type, frame_size,
            M6II_BURST_FRAMES, captured, M6II_BURST_INTERVAL_MS, ok);
        FIO_WriteFile(info, text, len);
        FIO_CloseFile(info);
    }

    DryosDebugMsg(0, 15,
        "M6II RAW burst: END frames=%u/%u size=%u ok=%d",
        captured, M6II_BURST_FRAMES, frame_size, ok);

    m6ii_burst_busy = 0;

    if (ok && captured == M6II_BURST_FRAMES)
        NotifyBox(8000, "RAW BURST RECORDED: %u frames -> %s", captured, M6II_BURST_FILE);
    else
        NotifyBox(8000, "RAW burst stopped: %u/%u frames. Send TXT + M6RB.",
                  captured, M6II_BURST_FRAMES);
}

static struct menu_entry m6ii_raw_burst_menu[] = {
    {
        .name   = "RECORD RAW burst (8 frames)",
        .priv   = m6ii_raw_burst_record,
        .select = run_in_separate_task,
        .help   = "Experimental first recorder: snapshots Canon 14-bit RAW buffer into a simple M6RB file. No EDMAC/MMIO."
    },
};

static void m6ii_raw_burst_init()
{
    menu_add("Debug", m6ii_raw_burst_menu, COUNT(m6ii_raw_burst_menu));
    DryosDebugMsg(0, 15, "M6II RAW burst recorder: menu registered");
}

INIT_FUNC(__FILE__, m6ii_raw_burst_init);

#endif /* !CONFIG_HELLO_WORLD */
