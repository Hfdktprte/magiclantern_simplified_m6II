/** \file
 * EOS M6 Mark II short RAM-buffered MLV recorder.
 *
 * Capture first, write second: this avoids SD-card latency from stretching the
 * frame cadence.  Still no EDMAC redirection/MMIO/SRM.  v2 replaces libc
 * memcpy on Canon's uncached RAW buffer with an explicitly unrolled 32-byte
 * word-copy loop so GCC can emit burst-friendly ARM loads/stores.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <propvalues.h>
#include <raw.h>
#include <timer.h>
#include "../../modules/raw_video/mlv_rec/mlv.h"

#define M6II_RAW_STATE_BASE       0x00010970
#define M6II_MLV_FRAMES           4
#define M6II_MLV_INTERVAL_MS      20
#define M6II_MLV_FILE             "M6II_FAST.MLV"
#define M6II_MLV_INFO             "M6II_FAST.TXT"

static volatile int m6ii_mlv_busy = 0;

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

/*
 * The first working MLV test showed libc memcpy taking about one second for a
 * 12.49 MB RAW frame.  Canon's RAW pointer is word aligned and this frame size
 * is divisible by 32, so copy eight words per iteration.  Keep the source
 * volatile: Canon is updating this buffer outside ML's control and we want
 * real memory reads, not compiler reuse.
 */
static void m6ii_fast_copy(void *dst_void, const void *src_void, uint32_t size)
{
    uint32_t *dst = (uint32_t *)dst_void;
    volatile const uint32_t *src = (volatile const uint32_t *)src_void;
    uint32_t words = size >> 2;

    while (words >= 8)
    {
        uint32_t a0 = src[0];
        uint32_t a1 = src[1];
        uint32_t a2 = src[2];
        uint32_t a3 = src[3];
        uint32_t a4 = src[4];
        uint32_t a5 = src[5];
        uint32_t a6 = src[6];
        uint32_t a7 = src[7];
        dst[0] = a0;
        dst[1] = a1;
        dst[2] = a2;
        dst[3] = a3;
        dst[4] = a4;
        dst[5] = a5;
        dst[6] = a6;
        dst[7] = a7;
        src += 8;
        dst += 8;
        words -= 8;
    }

    while (words--)
        *dst++ = *src++;
}

static void fill_raw_info(raw_info_t *ri, uint32_t w, uint32_t h, uint32_t frame_size)
{
    memset(ri, 0, sizeof(*ri));
    ri->api_version = 1;
    ri->buffer = 0;
    ri->height = h;
    ri->width = w;
    ri->pitch = (w * 14) / 8;
    ri->frame_size = frame_size;
    ri->bits_per_pixel = 14;
    ri->black_level = 2048;
    ri->white_level = 16200;
    ri->jpeg.x = 0;
    ri->jpeg.y = 0;
    ri->jpeg.width = w;
    ri->jpeg.height = h;
    ri->active_area.x1 = 0;
    ri->active_area.y1 = 0;
    ri->active_area.x2 = w;
    ri->active_area.y2 = h;
    ri->cfa_pattern = 0x02010100;
}

static void m6ii_record_fast_mlv()
{
    if (m6ii_mlv_busy)
    {
        NotifyBox(2000, "Fast MLV already running");
        return;
    }
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "Fast MLV requires active LiveView");
        return;
    }
    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_mlv_busy = 1;
    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);
    msleep(400);

    uint32_t w = state32(0x10);
    uint32_t h = state32(0x14);
    uint32_t raw_type = state32(0x44);
    uint32_t frame_size = 0;
    if (w && h && w < 0x10000 && h < 0x10000)
        frame_size = (w * h * 7) / 4;

    if (frame_size < 1024 * 1024 || frame_size > 80 * 1024 * 1024 || !state32(0x58))
    {
        call("lv_save_raw", 0);
        m6ii_mlv_busy = 0;
        NotifyBox(6000, "Fast MLV refused: %ux%u size=%u buf=%08x", w, h, frame_size, state32(0x58));
        return;
    }

    void *frames[M6II_MLV_FRAMES] = {0};
    uint64_t stamps[M6II_MLV_FRAMES] = {0};
    uint64_t copy_us[M6II_MLV_FRAMES] = {0};
    uint32_t srcs[M6II_MLV_FRAMES] = {0};
    uint32_t allocated = 0;

    for (uint32_t i = 0; i < M6II_MLV_FRAMES; i++)
    {
        frames[i] = malloc(frame_size);
        if (!frames[i])
            break;
        allocated++;
    }

    if (allocated < 2)
    {
        for (uint32_t i = 0; i < allocated; i++) free(frames[i]);
        call("lv_save_raw", 0);
        m6ii_mlv_busy = 0;
        NotifyBox(5000, "Fast MLV: insufficient RAM (%u buffers)", allocated);
        return;
    }

    uint64_t t0 = get_us_clock();
    uint32_t captured = 0;
    for (uint32_t i = 0; i < allocated; i++)
    {
        uint32_t src = state32(0x58);
        if (!src) break;
        stamps[i] = get_us_clock() - t0;
        srcs[i] = src;
        uint64_t c0 = get_us_clock();
        m6ii_fast_copy(frames[i], (void *)src, frame_size);
        copy_us[i] = get_us_clock() - c0;
        captured++;
        if (i + 1 < allocated)
            msleep(M6II_MLV_INTERVAL_MS);
    }

    int ret_off = call("lv_save_raw", 0);

    FILE *f = FIO_CreateFile(M6II_MLV_FILE);
    int ok = (f != 0);

    if (ok)
    {
        mlv_file_hdr_t mlvi;
        memset(&mlvi, 0, sizeof(mlvi));
        memcpy(mlvi.fileMagic, "MLVI", 4);
        mlvi.blockSize = sizeof(mlvi);
        memcpy(mlvi.versionString, MLV_VERSION_STRING, MIN(sizeof(mlvi.versionString), strlen(MLV_VERSION_STRING)));
        mlvi.fileGuid = t0;
        mlvi.fileNum = 0;
        mlvi.fileCount = 1;
        mlvi.videoClass = MLV_VIDEO_CLASS_RAW;
        mlvi.audioClass = 0;
        mlvi.videoFrameCount = captured;
        mlvi.sourceFpsNom = 1000000;
        mlvi.sourceFpsDenom = M6II_MLV_INTERVAL_MS * 1000;
        ok = write_all(f, &mlvi, sizeof(mlvi));

        mlv_rawi_hdr_t rawi;
        memset(&rawi, 0, sizeof(rawi));
        memcpy(rawi.blockType, "RAWI", 4);
        rawi.blockSize = sizeof(rawi);
        rawi.timestamp = 0;
        rawi.xRes = w;
        rawi.yRes = h;
        fill_raw_info(&rawi.raw_info, w, h, frame_size);
        if (ok) ok = write_all(f, &rawi, sizeof(rawi));

        for (uint32_t i = 0; ok && i < captured; i++)
        {
            mlv_vidf_hdr_t vidf;
            memset(&vidf, 0, sizeof(vidf));
            memcpy(vidf.blockType, "VIDF", 4);
            vidf.blockSize = sizeof(vidf) + frame_size;
            vidf.timestamp = stamps[i];
            vidf.frameNumber = i;
            vidf.frameSpace = 0;
            ok = write_all(f, &vidf, sizeof(vidf));
            if (ok) ok = write_all(f, frames[i], frame_size);
        }
        FIO_CloseFile(f);
    }

    for (uint32_t i = 0; i < allocated; i++) free(frames[i]);

    FILE *info = FIO_CreateFile(M6II_MLV_INFO);
    if (info)
    {
        char text[1024];
        int len = snprintf(text, sizeof(text),
            "M6II fast RAM MLV v2\n"
            "lv_set_mm_1=0x%08x\n"
            "lv_save_raw_1=0x%08x\n"
            "lv_save_raw_0=0x%08x\n"
            "width=%u\nheight=%u\nraw_type=0x%08x\nframe_size=%u\n"
            "allocated_buffers=%u\ncaptured_frames=%u\ninterval_ms=%u\nwrite_ok=%u\n"
            "src0=0x%08x\nsrc1=0x%08x\nsrc2=0x%08x\nsrc3=0x%08x\n"
            "t0_us=%llu\nt1_us=%llu\nt2_us=%llu\nt3_us=%llu\n"
            "copy0_us=%llu\ncopy1_us=%llu\ncopy2_us=%llu\ncopy3_us=%llu\n",
            ret_mm, ret_on, ret_off, w, h, raw_type, frame_size,
            allocated, captured, M6II_MLV_INTERVAL_MS, ok,
            srcs[0], srcs[1], srcs[2], srcs[3],
            stamps[0], stamps[1], stamps[2], stamps[3],
            copy_us[0], copy_us[1], copy_us[2], copy_us[3]);
        FIO_WriteFile(info, text, len);
        FIO_CloseFile(info);
    }

    m6ii_mlv_busy = 0;
    if (ok && captured >= 2)
        NotifyBox(8000, "FAST RAW MLV: %u frames -> %s", captured, M6II_MLV_FILE);
    else
        NotifyBox(8000, "Fast MLV failed: frames=%u write=%d", captured, ok);
}

static struct menu_entry m6ii_mlv_menu[] = {
    {
        .name   = "RECORD fast RAW MLV",
        .priv   = m6ii_record_fast_mlv,
        .select = run_in_separate_task,
        .help   = "4-frame MLV using an unrolled 32-byte CPU burst copy. Measures copy time. No EDMAC/MMIO."
    },
};

static void m6ii_mlv_init()
{
    menu_add("Debug", m6ii_mlv_menu, COUNT(m6ii_mlv_menu));
    DryosDebugMsg(0, 15, "M6II fast RAW MLV recorder: menu registered");
}

INIT_FUNC(__FILE__, m6ii_mlv_init);

#endif /* !CONFIG_HELLO_WORLD */
