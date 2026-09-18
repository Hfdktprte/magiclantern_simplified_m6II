/** \file
 * EOS M6 Mark II cached-alias RAW MLV test.
 *
 * Canon exposes the working RAW buffer through an uncached alias (0x64xxxxxx).
 * On the 2GB D8 mapping, the corresponding cacheable alias is address with
 * bit 30 cleared (0x24xxxxxx).  This test invalidates ONLY the RAW source cache
 * lines, then copies through the cacheable alias.  No EDMAC/MMIO/SRM writes.
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
#define M6II_CACHE_FRAMES         4
#define M6II_CACHE_INTERVAL_MS    20
#define M6II_CACHE_FILE           "M6II_CACHE.MLV"
#define M6II_CACHE_INFO           "M6II_CACHE.TXT"
#define M6II_DCACHE_LINE          32

static volatile int m6ii_cache_busy = 0;

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
        if (n != (int)chunk) return 0;
        done += chunk;
    }
    return 1;
}

/* Cortex-A9 DCIMVAC: invalidate one data-cache line by virtual address.
 * The RAW source is DMA-owned; ML never writes it through the cacheable alias,
 * so invalidation cannot discard ML-owned dirty RAW data. */
static inline void m6ii_dcache_invalidate_line(uint32_t addr)
{
    asm volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(addr) : "memory");
}

static void m6ii_dcache_invalidate_range(uint32_t addr, uint32_t size)
{
    uint32_t start = addr & ~(M6II_DCACHE_LINE - 1);
    uint32_t end = (addr + size + M6II_DCACHE_LINE - 1) & ~(M6II_DCACHE_LINE - 1);
    for (uint32_t p = start; p < end; p += M6II_DCACHE_LINE)
        m6ii_dcache_invalidate_line(p);
    asm volatile("dsb sy\n\tisb" : : : "memory");
}

static void m6ii_copy32(void *dst_void, const void *src_void, uint32_t size)
{
    uint32_t *dst = (uint32_t *)dst_void;
    const uint32_t *src = (const uint32_t *)src_void;
    uint32_t words = size >> 2;
    while (words >= 8)
    {
        uint32_t a0=src[0], a1=src[1], a2=src[2], a3=src[3];
        uint32_t a4=src[4], a5=src[5], a6=src[6], a7=src[7];
        dst[0]=a0; dst[1]=a1; dst[2]=a2; dst[3]=a3;
        dst[4]=a4; dst[5]=a5; dst[6]=a6; dst[7]=a7;
        src += 8; dst += 8; words -= 8;
    }
    while (words--) *dst++ = *src++;
}

static void fill_raw_info(raw_info_t *ri, uint32_t w, uint32_t h, uint32_t frame_size)
{
    memset(ri, 0, sizeof(*ri));
    ri->api_version = 1;
    ri->height = h;
    ri->width = w;
    ri->pitch = (w * 14) / 8;
    ri->frame_size = frame_size;
    ri->bits_per_pixel = 14;
    ri->black_level = 2048;
    ri->white_level = 16200;
    ri->jpeg.width = w;
    ri->jpeg.height = h;
    ri->active_area.x2 = w;
    ri->active_area.y2 = h;
    ri->cfa_pattern = 0x02010100;
}

static void m6ii_record_cached_mlv()
{
    if (m6ii_cache_busy) { NotifyBox(2000, "Cached MLV already running"); return; }
    if (!LV_NON_PAUSED) { NotifyBox(3000, "Cached MLV requires LiveView"); return; }
    if (RECORDING) { NotifyBox(3000, "Stop Canon recording first"); return; }

    m6ii_cache_busy = 1;
    int ret_mm = call("lv_set_mm", 1);
    int ret_on = call("lv_save_raw", 1);
    msleep(400);

    uint32_t w = state32(0x10), h = state32(0x14), raw_type = state32(0x44);
    uint32_t frame_size = (w && h && w < 0x10000 && h < 0x10000) ? (w*h*7)/4 : 0;
    uint32_t src_uncached = state32(0x58);
    if (!src_uncached || frame_size < 1024*1024 || frame_size > 80*1024*1024)
    {
        call("lv_save_raw", 0); m6ii_cache_busy = 0;
        NotifyBox(5000, "Cached MLV refused: %ux%u src=%08x size=%u", w,h,src_uncached,frame_size);
        return;
    }

    void *frames[M6II_CACHE_FRAMES] = {0};
    uint64_t stamps[M6II_CACHE_FRAMES] = {0};
    uint32_t inv_us[M6II_CACHE_FRAMES] = {0};
    uint32_t copy_us[M6II_CACHE_FRAMES] = {0};
    uint32_t srcs[M6II_CACHE_FRAMES] = {0};
    uint32_t allocated = 0;
    for (uint32_t i=0; i<M6II_CACHE_FRAMES; i++)
    {
        frames[i] = malloc(frame_size);
        if (!frames[i]) break;
        allocated++;
    }
    if (allocated < 2)
    {
        for (uint32_t i=0;i<allocated;i++) free(frames[i]);
        call("lv_save_raw",0); m6ii_cache_busy=0;
        NotifyBox(5000,"Cached MLV: insufficient RAM (%u)",allocated); return;
    }

    uint64_t t0 = get_us_clock();
    uint32_t captured = 0;
    for (uint32_t i=0; i<allocated; i++)
    {
        src_uncached = state32(0x58);
        if (!src_uncached) break;
        uint32_t src_cached = src_uncached & ~0x40000000u;
        srcs[i] = src_uncached;
        stamps[i] = get_us_clock() - t0;
        uint64_t a = get_us_clock();
        m6ii_dcache_invalidate_range(src_cached, frame_size);
        uint64_t b = get_us_clock();
        m6ii_copy32(frames[i], (void *)src_cached, frame_size);
        uint64_t c = get_us_clock();
        inv_us[i] = (uint32_t)(b-a);
        copy_us[i] = (uint32_t)(c-b);
        captured++;
        if (i+1 < allocated) msleep(M6II_CACHE_INTERVAL_MS);
    }

    int ret_off = call("lv_save_raw",0);
    FILE *f = FIO_CreateFile(M6II_CACHE_FILE);
    int ok = (f != 0);
    if (ok)
    {
        mlv_file_hdr_t mlvi; memset(&mlvi,0,sizeof(mlvi));
        memcpy(mlvi.fileMagic,"MLVI",4); mlvi.blockSize=sizeof(mlvi);
        memcpy(mlvi.versionString,MLV_VERSION_STRING,MIN(sizeof(mlvi.versionString),strlen(MLV_VERSION_STRING)));
        mlvi.fileGuid=t0; mlvi.fileCount=1; mlvi.videoClass=MLV_VIDEO_CLASS_RAW;
        mlvi.videoFrameCount=captured; mlvi.sourceFpsNom=1000000; mlvi.sourceFpsDenom=M6II_CACHE_INTERVAL_MS*1000;
        ok=write_all(f,&mlvi,sizeof(mlvi));

        mlv_rawi_hdr_t rawi; memset(&rawi,0,sizeof(rawi));
        memcpy(rawi.blockType,"RAWI",4); rawi.blockSize=sizeof(rawi); rawi.xRes=w; rawi.yRes=h;
        fill_raw_info(&rawi.raw_info,w,h,frame_size);
        if(ok) ok=write_all(f,&rawi,sizeof(rawi));
        for(uint32_t i=0; ok && i<captured; i++)
        {
            mlv_vidf_hdr_t v; memset(&v,0,sizeof(v));
            memcpy(v.blockType,"VIDF",4); v.blockSize=sizeof(v)+frame_size; v.timestamp=stamps[i]; v.frameNumber=i;
            ok=write_all(f,&v,sizeof(v)); if(ok) ok=write_all(f,frames[i],frame_size);
        }
        FIO_CloseFile(f);
    }
    for(uint32_t i=0;i<allocated;i++) free(frames[i]);

    FILE *info=FIO_CreateFile(M6II_CACHE_INFO);
    if(info)
    {
        char text[1200];
        int len=snprintf(text,sizeof(text),
            "M6II cached RAW MLV v3\n"
            "lv_set_mm_1=0x%08x\nlv_save_raw_1=0x%08x\nlv_save_raw_0=0x%08x\n"
            "width=0x%08x\nheight=0x%08x\nraw_type=0x%08x\nframe_size=0x%08x\n"
            "allocated=0x%08x\ncaptured=0x%08x\nwrite_ok=0x%08x\n"
            "src0=0x%08x\nsrc1=0x%08x\nsrc2=0x%08x\nsrc3=0x%08x\n"
            "stamp0_lo=0x%08x\nstamp1_lo=0x%08x\nstamp2_lo=0x%08x\nstamp3_lo=0x%08x\n"
            "inv0_us=0x%08x\ninv1_us=0x%08x\ninv2_us=0x%08x\ninv3_us=0x%08x\n"
            "copy0_us=0x%08x\ncopy1_us=0x%08x\ncopy2_us=0x%08x\ncopy3_us=0x%08x\n",
            ret_mm,ret_on,ret_off,w,h,raw_type,frame_size,allocated,captured,ok,
            srcs[0],srcs[1],srcs[2],srcs[3],
            (uint32_t)stamps[0],(uint32_t)stamps[1],(uint32_t)stamps[2],(uint32_t)stamps[3],
            inv_us[0],inv_us[1],inv_us[2],inv_us[3],copy_us[0],copy_us[1],copy_us[2],copy_us[3]);
        FIO_WriteFile(info,text,len); FIO_CloseFile(info);
    }

    m6ii_cache_busy=0;
    NotifyBox(8000, ok ? "CACHED RAW MLV: %u frames" : "Cached MLV failed: %u frames", captured);
}

static struct menu_entry m6ii_cache_menu[] = {{
    .name="RECORD cached RAW MLV",
    .priv=m6ii_record_cached_mlv,
    .select=run_in_separate_task,
    .help="Invalidate RAW cache lines then copy via cacheable alias. No EDMAC/MMIO."
}};

static void m6ii_cache_init(){ menu_add("Debug",m6ii_cache_menu,COUNT(m6ii_cache_menu)); }
INIT_FUNC(__FILE__,m6ii_cache_init);

#endif
