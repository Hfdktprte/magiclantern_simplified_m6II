#include <module.h>
#include <dryos.h>
#include <mem_defs.h>
#include <mem.h>
#include <edmac-memcpy.h>

#include "crx_encoder.h"

struct crx_encode_descriptor
{
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t bits_per_pixel;
    uint32_t source;
    uint32_t stride_pixels;
    uint32_t split_path;
    uint32_t unused[6];
};
SIZE_CHECK_STRUCT(crx_encode_descriptor, 0x3c);

extern uint32_t algsmgr_start_encode(
    struct crx_encode_descriptor *descriptors,
    uint32_t count,
    void (*completion)(void *context, void **handles, uint32_t count),
    void *context);
extern uint32_t algsmgr_get_ring_list(
    void *handle, uint32_t *count, void ***addresses, uint32_t **sizes);
extern uint32_t algsmgr_get_main_buffer(
    void *handle, void **address, uint32_t *size);
extern uint32_t algsmgr_free_enc_buffer(
    void *handle, uint32_t arg1, uint32_t arg2);
extern void edmac_memcpy_res_lock(void);
extern void edmac_memcpy_res_unlock(void);

static struct semaphore *crx_sem;
static volatile void *crx_handle;
static volatile uint32_t crx_handle_count;

static int crx_stage_frame(
    void *source, uint32_t source_pitch,
    uint32_t crop_x_bytes, uint32_t crop_y,
    uint32_t pitch, uint32_t height)
{
    void *started;

    edmac_memcpy_res_lock();
    started = edmac_copy_rectangle_cbr_start(
        (void *)0x80300000u, source,
        source_pitch, crop_x_bytes, crop_y,
        pitch, 0, 0, pitch, height,
        0, 0, 0);
    edmac_memcpy_res_unlock();
    return started != 0;
}

static void crx_complete(void *context, void **handles, uint32_t count)
{
    (void) context;
    crx_handle_count = count;
    crx_handle = count ? handles[0] : 0;
    give_semaphore(crx_sem);
}

static int crx_buffer_valid(void *address, uint32_t size, uint32_t limit)
{
    uint32_t raw = (uint32_t) address;
    uint32_t base = (uint32_t) CACHEABLE(address);
    int conventional_ram =
        base >= 0x1000 && base < 0x40000000 &&
        size <= 0x40000000 - base;
    int crx_ram =
        (raw >= 0x80000000 && raw < 0x90000000 &&
         size <= 0x90000000 - raw) ||
        (raw >= 0xB0000000 && raw < 0xC0000000 &&
         size <= 0xC0000000 - raw);
    return address && size && size <= limit &&
           (conventional_ram || crx_ram);
}

static int crx_ranges_overlap(
    void *first, uint32_t first_size, void *second, uint32_t second_size)
{
    uint32_t first_base = (uint32_t) CACHEABLE(first);
    uint32_t second_base = (uint32_t) CACHEABLE(second);
    return first_base < second_base + second_size &&
           second_base < first_base + first_size;
}

static int crx_copy_fragment(
    void *destination, const void *source, uint32_t size)
{
    uint32_t raw = (uint32_t) source;

    if (raw < 0x80000000u)
    {
        memcpy(destination, source, size);
        return 1;
    }

    uint32_t offset = 0;
    uint32_t rows = size / 0xF000u;
    int attempt;

    if (rows)
    {
        for (attempt = 0; attempt < 2; attempt++)
        {
            if (edmac_copy_rectangle_cbr_start(
                    UNCACHEABLE(destination), (void *)source,
                    0xF000u, 0, 0, 0xF000u, 0, 0,
                    0xF000u, rows, 0, 0, 0))
                break;
            msleep(1);
        }
        if (attempt == 2) return 0;
        offset = rows * 0xF000u;
    }

    uint32_t amount = size - offset;
    if (!amount) return 1;
    for (attempt = 0; attempt < 2; attempt++)
    {
        if (edmac_copy_rectangle_cbr_start(
                UNCACHEABLE((uint8_t *)destination + offset),
                (uint8_t *)source + offset,
                amount, 0, 0, amount, 0, 0, amount, 1,
                0, 0, 0))
            return 1;
        msleep(1);
    }
    return 0;
}

enum
{
    CRX_COPY_OK,
    CRX_COPY_ACCESSOR_ERROR,
    CRX_COPY_METADATA_ERROR,
    CRX_COPY_FRAGMENT_ERROR,
    CRX_COPY_TOO_LARGE,
    CRX_COPY_OVERLAP
};

static int crx_copy_result(
    void *handle, void *destination, uint32_t destination_size,
    uint32_t *total_size, int *copy_status)
{
    void *main_address = 0;
    uint32_t main_size = 0;
    uint32_t ring_count = 0;
    void **ring_addresses = 0;
    uint32_t *ring_sizes = 0;
    uint32_t capacity = 0;
    uint32_t offset;
    uint32_t i;

    *copy_status = CRX_COPY_OK;
    if (algsmgr_get_main_buffer(handle, &main_address, &main_size) != 0 ||
        algsmgr_get_ring_list(
            handle, &ring_count, &ring_addresses, &ring_sizes) != 0)
    {
        *copy_status = CRX_COPY_ACCESSOR_ERROR;
        return 0;
    }

    if (!crx_buffer_valid(main_address, main_size, destination_size) ||
        (ring_count &&
         ((uint32_t) ring_sizes <= (uint32_t) ring_addresses ||
          (uint32_t) ring_sizes - (uint32_t) ring_addresses > 0x1000 ||
          ((uint32_t) ring_sizes - (uint32_t) ring_addresses) & 3)))
    {
        *copy_status = CRX_COPY_METADATA_ERROR;
        return 0;
    }

    if (ring_count)
    {
        capacity = ((uint32_t) ring_sizes -
                    (uint32_t) ring_addresses) / sizeof(void *);
        if (ring_count > capacity ||
            !crx_buffer_valid(
                ring_addresses, ring_count * sizeof(void *), 0x1000) ||
            !crx_buffer_valid(
                ring_sizes, ring_count * sizeof(uint32_t), 0x1000))
        {
            *copy_status = CRX_COPY_METADATA_ERROR;
            return 0;
        }
    }

    *total_size = main_size;
    for (i = 0; i < ring_count; i++)
    {
        void *address = ring_addresses[i];
        uint32_t size = ring_sizes[i];
        if (!address && !size) continue;
        if (!crx_buffer_valid(address, size, destination_size))
        {
            *copy_status = CRX_COPY_FRAGMENT_ERROR;
            return 0;
        }
        if (*total_size > destination_size - size)
        {
            *copy_status = CRX_COPY_TOO_LARGE;
            return 0;
        }
        *total_size += size;
    }

    if (crx_ranges_overlap(
            destination, *total_size, main_address, main_size))
    {
        *copy_status = CRX_COPY_OVERLAP;
        return 0;
    }
    for (i = 0; i < ring_count; i++)
    {
        void *address = ring_addresses[i];
        uint32_t size = ring_sizes[i];
        if (!address && !size) continue;
        if (crx_ranges_overlap(destination, *total_size, address, size))
        {
            *copy_status = CRX_COPY_OVERLAP;
            return 0;
        }
    }

    memcpy(destination, main_address, main_size);
    offset = main_size;
    if (ring_count) edmac_memcpy_res_lock();
    for (i = 0; i < ring_count; i++)
    {
        void *address = ring_addresses[i];
        uint32_t size = ring_sizes[i];
        if (!address && !size) continue;
        if (!crx_copy_fragment(
                (uint8_t *) destination + offset, address, size))
        {
            edmac_memcpy_res_unlock();
            *copy_status = CRX_COPY_FRAGMENT_ERROR;
            return 0;
        }
        offset += size;
    }
    if (ring_count) edmac_memcpy_res_unlock();
    return 1;
}

#define CRX_ENGINE_CONFIG_POINTER 0xE750u

struct crx_engine_geometry
{
    volatile uint32_t *config;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;
};

static int crx_set_engine_geometry(
    uint32_t width, uint32_t height,
    struct crx_engine_geometry *saved)
{
    uint32_t address = MEM(CRX_ENGINE_CONFIG_POINTER);

    if (address < 0x40001000u || address > 0x7ffffc30u || (address & 3u))
        return 0;

    volatile uint32_t *config = (volatile uint32_t *) address;
    saved->config = config;
    saved->stride = config[0x30 / 4];
    saved->width = config[0x200 / 4];
    saved->height = config[0x204 / 4];
    saved->offset_x = config[0x2c8 / 4];
    saved->offset_y = config[0x2cc / 4];
    config[0x30 / 4] = width;
    config[0x200 / 4] = width;
    config[0x204 / 4] = height;
    config[0x2c8 / 4] = 0;
    config[0x2cc / 4] = 0;
    return 1;
}

static void crx_restore_engine_geometry(
    const struct crx_engine_geometry *saved)
{
    volatile uint32_t *config = saved->config;

    if (!config) return;
    config[0x30 / 4] = saved->stride;
    config[0x200 / 4] = saved->width;
    config[0x204 / 4] = saved->height;
    config[0x2c8 / 4] = saved->offset_x;
    config[0x2cc / 4] = saved->offset_y;
}

int crx_encode_frame(
    void *source, uint32_t source_width, uint32_t source_height,
    uint32_t source_pitch,
    uint32_t crop_x, uint32_t crop_y, uint32_t width, uint32_t height,
    void *destination, uint32_t destination_size,
    uint32_t *compressed_size)
{
    struct crx_encode_descriptor descriptor;
    struct crx_engine_geometry geometry = { 0 };
    uint32_t total_size = 0;
    uint32_t status;
    int copy_status;

    if (compressed_size) *compressed_size = 0;
    if (!source || !destination || !compressed_size ||
        !source_width || !source_height || !width || !height ||
        (source_width & 7u) || (width & 7u) || (crop_x & 7u) ||
        source_pitch < source_width * 14u / 8u ||
        crop_x + width > source_width || crop_y + height > source_height ||
        destination_size < 512u)
        return -1;

    uint32_t pitch = width * 14u / 8u;
    if (!crx_sem)
        crx_sem = create_named_semaphore("crx_active", SEM_CREATE_LOCKED);
    if (!crx_sem) return -2;
    while (take_semaphore_now(crx_sem) == 0) {}

    if (!crx_stage_frame(
            source, source_pitch, crop_x * 14u / 8u, crop_y,
            pitch, height))
        return -19;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.width = width;
    descriptor.height = height;
    descriptor.bits_per_pixel = 14;
    descriptor.source = 0x80300000u;
    descriptor.stride_pixels = width;
    descriptor.unused[0] = 0x001d52c0u;
    descriptor.unused[1] = 0xe0109595u;
    descriptor.unused[3] = 0xe0964910u;
    descriptor.unused[4] = 0xe0964910u;
    descriptor.unused[5] = 0xffffffffu;

    if (!crx_set_engine_geometry(width, height, &geometry)) return -20;

    crx_handle = 0;
    crx_handle_count = 0;
    status = algsmgr_start_encode(&descriptor, 1, crx_complete, 0);
    if (status & 1u)
    {
        crx_restore_engine_geometry(&geometry);
        return -3;
    }
    if (take_semaphore(crx_sem, 1000) != 0)
    {
        crx_restore_engine_geometry(&geometry);
        return -4;
    }
    crx_restore_engine_geometry(&geometry);
    if (crx_handle_count != 1 || !crx_handle) return -5;

    void *handle = (void *) crx_handle;
    if (!crx_copy_result(
            handle, destination, destination_size,
            &total_size, &copy_status))
    {
        algsmgr_free_enc_buffer(handle, 0, 0);
        return -6 - copy_status;
    }
    if (algsmgr_free_enc_buffer(handle, 0, 0) & 1u) return -16;

    *compressed_size = total_size;
    return 0;
}
