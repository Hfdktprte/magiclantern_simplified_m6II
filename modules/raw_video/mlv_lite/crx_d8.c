/*
 * EOS M6 Mark II / DIGIC 8 native CRX encoder proof of concept.
 *
 * Addresses and parameter layout are for firmware 1.1.1 only.
 * The backend deliberately stages the selected MLV crop into a contiguous
 * 14-bit buffer.  This avoids depending on still-unmapped CRX input-stride
 * behaviour and keeps the first implementation fail-closed.
 */

#include "dryos.h"
#include "module.h"
#include "mem.h"
#include "raw.h"
#include "crx_d8.h"

#define M6II_CRAW_DIRECT_ENC_INIT       ((crx_init_fn)      0xE0300ED1)
#define M6II_CRAW_DIRECT_ENC_SET_PARAM  ((crx_setparam_fn)  0xE0301051)
#define M6II_CRAW_DIRECT_ENC_START      ((crx_start_fn)     0xE0301307)
#define M6II_CRAW_CREATE_MAIN_HEADER    ((crx_mainhdr_fn)   0xE02FF26F)
#define M6II_CRAW_CREATE_SUB_HEADER     ((crx_subhdr_fn)    0xE02FF561)

#define CRX_ENCODER_ID          0
#define CRX_INIT_SIZE           0x58
#define CRX_MAIN_HEADER_MAX     0x80
#define CRX_SUB_HEADER_MAX      0x600
#define CRX_RESULT_MAX          16
#define CRX_OUTPUT_FACTOR_NUM   2
#define CRX_OUTPUT_FACTOR_DEN   1
#define CRX_FRAME_GUARD         0x1000
#define CRX_ALIGN               0x40
#define CRX_TIMEOUT_MS          1000

/*
 * The direct path below is restricted to Canon's mode-0, single-tile lossless
 * configuration reconstructed from the M6 II 1.1.1 ROM.  In that path:
 *   encoder id = 0
 *   DwtLevel   = 0
 *   ctx mode   = 1
 *   one tile is used while frame width < 4096
 *
 * CRawDirectEncStart forwards its third argument unchanged to the completion
 * callback, so a module-owned cookie is valid and is checked there.
 */
#define CRX_D8_DIRECT_POC 1

struct crx_buf_desc
{
    void *ptr;
    uint32_t size;
};

/*
 * CRawDirectEncStart consumes a record array in pairs.  For the no-wrap case
 * Canon's builder emits six words per logical output record.
 */
struct crx_start_record
{
    /* Canon's 12-byte DEnc buffer descriptor: offset/reserved, capacity, address. */
    uint32_t offset0;
    uint32_t capacity0;
    uint32_t address0;

    /* Optional second span (used for wrapped/ring buffers); zero for POC. */
    uint32_t offset1;
    uint32_t capacity1;
    uint32_t address1;
};

struct crx_start_desc
{
    uint32_t count;                  /* two units per 24-byte record */
    struct crx_start_record *record;
};

struct crx_result
{
    uint32_t count;
    uint32_t *addresses;
    uint32_t *sizes;
};

struct crx_set_param
{
    uint8_t  type;
    uint8_t  pad01;
    uint16_t m0;
    uint16_t m1;
    uint16_t m2;
    uint16_t m3;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t pad0e;
    uint32_t image_addr;
    uint32_t dup;
};

typedef void (*crx_init_fn)(uint32_t id, const void *param);
typedef void (*crx_setparam_fn)(uint32_t id, const struct crx_set_param *param);
typedef void (*crx_start_fn)(uint32_t id, struct crx_start_desc *desc, void *cookie);
typedef uint32_t (*crx_mainhdr_fn)(uint32_t id, uint32_t zero1,
                                   struct crx_buf_desc *desc,
                                   uint32_t zero2, uint32_t zero3);
typedef uint32_t (*crx_subhdr_fn)(uint32_t id, struct crx_buf_desc *desc,
                                  uint32_t zero);

static struct semaphore *crx_sem = 0;
static volatile uint32_t crx_done_valid = 0;
static volatile uint32_t crx_result_count = 0;
static uint32_t crx_result_addresses[CRX_RESULT_MAX];
static uint32_t crx_result_sizes[CRX_RESULT_MAX];

/*
 * CRawDirectEncStart stores this opaque value and forwards it unchanged as the
 * sixth completion-callback argument.  It is not dereferenced by the encoder.
 */
static uint32_t crx_callback_cookie = 0x43525838; /* "CRX8" */

static uint8_t crx_main_header_buf[CRX_MAIN_HEADER_MAX] __attribute__((aligned(64)));
static uint32_t crx_main_header_size = 0;
static int crx_active = 0;
static int crx_width = 0;
static int crx_height = 0;
static void *crx_staging_alloc = 0;
static uint8_t *crx_staging = 0;
static uint32_t crx_staging_size = 0;
static int crx_hw_initialized = 0;
static int crx_hw_width = 0;
static int crx_hw_height = 0;

static inline void put_u16(uint8_t *p, uint32_t off, uint16_t v)
{
    *(uint16_t *)(p + off) = v;
}

static inline void put_u32(uint8_t *p, uint32_t off, uint32_t v)
{
    *(uint32_t *)(p + off) = v;
}

/*
 * Compact an encoded segment downward.  In this backend destination is always
 * below source; copying forward is overlap-safe for that exact relationship.
 */
static void crx_move_down(uint8_t *dst, const uint8_t *src, uint32_t size)
{
    uint32_t i;
    ASSERT(dst <= src);
    for (i = 0; i < size; i++)
        dst[i] = src[i];
}

/*
 * Canon invokes the +0x48 callback for encoder-side status events.
 * Keep this callback intentionally inert for the POC.
 */
static void crx_status_cb(uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
}

/*
 * CRawDirectEncIntrPostProcess invokes the +0x4c user callback with six AAPCS
 * arguments.  The fourth argument (r3) is Canon's result object.
 * Copy the arrays before returning because the object belongs to Canon.
 */
static void crx_complete_cb(uint32_t id, uint32_t sequence, uint32_t status,
                            struct crx_result *result,
                            uint32_t opaque0, uint32_t opaque1)
{
    (void)id; (void)sequence; (void)status; (void)opaque0;

    if (opaque1 != (uint32_t)&crx_callback_cookie)
        return;

    crx_done_valid = 0;
    crx_result_count = 0;

    if (result && result->addresses && result->sizes &&
        result->count > 0 && result->count <= CRX_RESULT_MAX)
    {
        uint32_t i;
        for (i = 0; i < result->count; i++)
        {
            crx_result_addresses[i] = result->addresses[i];
            crx_result_sizes[i] = result->sizes[i];
        }
        crx_result_count = result->count;
        crx_done_valid = 1;
    }

    if (crx_sem)
        give_semaphore(crx_sem);
}

int crx_d8_supported(void)
{
#if CRX_D8_DIRECT_POC
    return is_camera("M6II", "1.1.1");
#else
    return 0;
#endif
}

static uint32_t crx_raw_bytes(int width, int height)
{
    return (uint32_t)width * (uint32_t)height * 14u / 8u;
}

uint32_t crx_d8_slot_payload_capacity(int width, int height)
{
    uint32_t raw = crx_raw_bytes(width, height);
    uint32_t encoded = raw * CRX_OUTPUT_FACTOR_NUM / CRX_OUTPUT_FACTOR_DEN;
    uint32_t total = CRX_SUB_HEADER_MAX + CRX_ALIGN + encoded + CRX_FRAME_GUARD;
    return ALIGN_UP(total, 4096);
}

static void crx_build_init_param(uint8_t p[CRX_INIT_SIZE], int width, int height)
{
    memset(p, 0, CRX_INIT_SIZE);

    /* Canon CRawDirectEncInit parameter block, FW 1.1.1. */
    put_u16(p, 0x00, (uint16_t)width);
    put_u16(p, 0x02, (uint16_t)height);

    /*
     * Single full-frame tile for the first POC.  The low-level encoder accepts
     * >=44 pixels; using the complete image removes tile-edge ambiguity.
     */
    put_u16(p, 0x04, (uint16_t)width);
    put_u16(p, 0x06, (uint16_t)height);

    p[0x08] = 14;                 /* BitDepth */
    put_u32(p, 0x0c, 0);
    put_u32(p, 0x10, 0);          /* DwtLevel=0: lossless CRX */
    put_u32(p, 0x14, 0);
    put_u32(p, 0x18, 0);
    put_u32(p, 0x1c, 0);
    put_u32(p, 0x20, 0);
    /* Canon mode-map E02ACDE8, mode code 0, root flags clear. */
    put_u32(p, 0x24, 0);          /* mode-dependent field: mode 0 => 0 */
    put_u32(p, 0x28, 0);          /* table +0x20 in Canon's builder */
    put_u32(p, 0x2c, 1);          /* Canon ctx +0x2c for mode 0 */
    put_u32(p, 0x30, 0x10);
    p[0x34] = 8;

    /* Canon puts its status callback at +0x48 and result callback at +0x4c. */
    put_u32(p, 0x48, (uint32_t)crx_status_cb);
    put_u32(p, 0x4c, (uint32_t)crx_complete_cb);
    put_u32(p, 0x50, 0);          /* root +0xd8 bit 0, clear in mode-0 POC */

    /* Canon: (BitDepth * table_entry[+0x1c]) >> 3; for contiguous input,
     * table_entry[+0x1c] is the active width in samples. */
    put_u16(p, 0x54, (uint16_t)(((uint32_t)width * 14u) >> 3));
}

int crx_d8_start_recording(int width, int height)
{
    uint8_t init_param[CRX_INIT_SIZE] __attribute__((aligned(8)));
    struct crx_buf_desc main_desc;
    uint32_t main_size;

    if (!crx_d8_supported())
        return 0;

    /* Canon mode-0 builder uses one tile below 4096 pixels.  Keep the first
     * enabled POC inside that proven geometry instead of guessing multi-tile. */
    if (width < 44 || height < 44 || width >= 4096 || height > 0xffff)
        return 0;

    if ((width & 7) || (height & 1))
        return 0;

    if (crx_hw_initialized && (width != crx_hw_width || height != crx_hw_height))
    {
        printf("[CRX] restart geometry differs from initialized encoder\n");
        return 0;
    }

    if (!crx_sem)
        crx_sem = create_named_semaphore("crx_d8_sem", SEM_CREATE_LOCKED);

    if (!crx_sem)
        return 0;

    if (crx_staging_alloc)
    {
        fio_free(crx_staging_alloc);
        crx_staging_alloc = 0;
        crx_staging = 0;
        crx_staging_size = 0;
    }

    crx_staging_size = crx_raw_bytes(width, height);
    crx_staging_alloc = fio_malloc(crx_staging_size + CRX_ALIGN);
    if (!crx_staging_alloc)
    {
        printf("[CRX] staging alloc failed: %u\n", crx_staging_size);
        return 0;
    }
    crx_staging = (uint8_t *)ALIGN_UP((uintptr_t)UNCACHEABLE(crx_staging_alloc), CRX_ALIGN);

    crx_done_valid = 0;
    crx_result_count = 0;
    crx_main_header_size = 0;

    if (!crx_hw_initialized)
    {
        crx_build_init_param(init_param, width, height);
        M6II_CRAW_DIRECT_ENC_INIT(CRX_ENCODER_ID, init_param);
        crx_hw_initialized = 1;
        crx_hw_width = width;
        crx_hw_height = height;
    }

    main_desc.ptr = UNCACHEABLE(crx_main_header_buf);
    main_desc.size = sizeof(crx_main_header_buf);
    main_size = M6II_CRAW_CREATE_MAIN_HEADER(CRX_ENCODER_ID, 0,
                                             &main_desc, 0, 0);

    if (!main_size || main_size > sizeof(crx_main_header_buf))
    {
        printf("[CRX] main header failed: %u\n", main_size);
        return 0;
    }

    /*
     * Header was written through the uncached alias.  Copy back through the
     * same alias so later FIO writes from the normal symbol see stable data.
     */
    memcpy(crx_main_header_buf, UNCACHEABLE(crx_main_header_buf), main_size);

    crx_main_header_size = main_size;
    crx_width = width;
    crx_height = height;
    crx_active = 1;

    printf("[CRX] D8 encoder ready: %dx%d, main=%u\n",
           width, height, main_size);

    return 1;
}

void crx_d8_stop_recording(void)
{
    /*
     * No standalone CRawDirectEncTerminate entry is required between frames in
     * Canon's direct path.  Leave the engine initialized for the session and
     * fail closed on the next start if geometry changes.
     */
    crx_active = 0;
    crx_width = 0;
    crx_height = 0;
    crx_done_valid = 0;
    crx_result_count = 0;

    if (crx_staging_alloc)
    {
        fio_free(crx_staging_alloc);
        crx_staging_alloc = 0;
        crx_staging = 0;
        crx_staging_size = 0;
    }
}

const void *crx_d8_main_header(uint32_t *size)
{
    if (size)
        *size = crx_main_header_size;
    return crx_main_header_size ? crx_main_header_buf : 0;
}

int crx_d8_compress_raw_rectangle(
    void *dst, uint32_t dst_capacity,
    const void *src, int src_pitch,
    int src_x, int src_y,
    int width, int height)
{
    uint32_t raw_bytes;
    uint32_t row_bytes;
    uint32_t encoded_capacity;
    uint8_t *base;
    uint8_t *encoded_base;
    struct crx_start_record record __attribute__((aligned(16)));
    struct crx_start_desc start_desc;
    struct crx_set_param setp;
    struct crx_buf_desc sub_desc;
    uint32_t sub_size;
    uint32_t payload_size = 0;
    uint32_t i;
    uint8_t *final_dst;
    int err;

    if (!crx_active || !dst || !src)
        return -1;

    if (width != crx_width || height != crx_height)
        return -2;

    if ((src_x & 7) || (src_y & 1) || src_pitch <= 0)
        return -3;

    if (dst_capacity < crx_d8_slot_payload_capacity(width, height))
        return -4;

    raw_bytes = crx_raw_bytes(width, height);
    row_bytes = (uint32_t)width * 14u / 8u;
    encoded_capacity = raw_bytes * CRX_OUTPUT_FACTOR_NUM / CRX_OUTPUT_FACTOR_DEN;

    base = (uint8_t *)UNCACHEABLE(dst);
    encoded_base = (uint8_t *)ALIGN_UP((uintptr_t)(base + CRX_SUB_HEADER_MAX),
                                       CRX_ALIGN);
    encoded_capacity &= ~(CRX_ALIGN - 1);
    if (!crx_staging || crx_staging_size < raw_bytes)
        return -5;

    /* Pack the selected crop into a contiguous 14-bit frame for the encoder. */
    {
        const uint8_t *src8 = (const uint8_t *)src;
        uint32_t src_x_bytes = (uint32_t)src_x * 14u / 8u;
        int y;
        for (y = 0; y < height; y++)
        {
            const void *s = src8 + (src_y + y) * src_pitch + src_x_bytes;
            void *d = crx_staging + (uint32_t)y * row_bytes;
            memcpy(d, s, row_bytes);
        }
    }

    memset(&setp, 0, sizeof(setp));
    setp.type = 0;
    setp.image_addr = (uint32_t)crx_staging;
    setp.x_offset = 0;
    setp.y_offset = 0;
    setp.dup = 0;

    M6II_CRAW_DIRECT_ENC_SET_PARAM(CRX_ENCODER_ID, &setp);

    /* E0301306 walks the descriptor count in steps of two; one lossless
     * output record is therefore two 12-byte spans.  The second span is the
     * optional ring-wrap segment and remains zero for this contiguous POC. */
    memset(&record, 0, sizeof(record));
    encoded_capacity &= ~(CRX_ALIGN - 1);
    if (encoded_capacity < 0x1000)
        return -6;

    record.offset0 = 0;
    record.capacity0 = encoded_capacity;
    record.address0 = (uint32_t)encoded_base;

    start_desc.count = 2;           /* one 24-byte logical output record */
    start_desc.record = &record;

    crx_done_valid = 0;
    crx_result_count = 0;

    M6II_CRAW_DIRECT_ENC_START(CRX_ENCODER_ID, &start_desc,
                               &crx_callback_cookie);

    err = take_semaphore(crx_sem, CRX_TIMEOUT_MS);
    if (err)
    {
        printf("[CRX] timeout\n");
        return -7;
    }

    if (!crx_done_valid || !crx_result_count)
    {
        printf("[CRX] completion without result\n");
        return -8;
    }

    for (i = 0; i < crx_result_count; i++)
    {
        if (!crx_result_addresses[i] || !crx_result_sizes[i])
            return -9;
        if (crx_result_addresses[i] < (uint32_t)encoded_base || crx_result_sizes[i] > encoded_capacity || crx_result_addresses[i] > (uint32_t)encoded_base + encoded_capacity - crx_result_sizes[i])
            return -10;
        if (payload_size > encoded_capacity - crx_result_sizes[i])
            return -10;
        payload_size += crx_result_sizes[i];
    }

    sub_desc.ptr = base;
    sub_desc.size = CRX_SUB_HEADER_MAX;
    sub_size = M6II_CRAW_CREATE_SUB_HEADER(CRX_ENCODER_ID, &sub_desc, 0);

    if (!sub_size || sub_size > CRX_SUB_HEADER_MAX)
    {
        printf("[CRX] subheader failed: %u\n", sub_size);
        return -11;
    }

    if (sub_size + payload_size > dst_capacity)
        return -12;

    final_dst = base + sub_size;
    for (i = 0; i < crx_result_count; i++)
    {
        crx_move_down(final_dst,
                      (const uint8_t *)crx_result_addresses[i],
                      crx_result_sizes[i]);
        final_dst += crx_result_sizes[i];
    }

    return (int)(sub_size + payload_size);
}
