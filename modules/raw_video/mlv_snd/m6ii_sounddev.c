/**
 * EOS M6 Mark II compatibility backend for mlv_snd.
 *
 * DIGIC 8 no longer exposes the legacy StartASIFDMAADC API used by mlv_snd.
 * Canon's own movie audio path uses SoundDev/AStream instead.  This backend
 * keeps Canon's native recorder and its DMA buffers intact, then copies each
 * completed PCM span into the legacy buffers supplied by mlv_snd.
 *
 * M6II 1.1.1 ROM mapping used here:
 *   0xE03B884E  SoundDev native record-stream setup
 *   0xE03B8932  SoundDev movie/record stream start
 *   0xE03B894E  SoundDev movie/record stream stop
 *
 * The setup routine validates (rate,bits,channels,endian) through
 * 0xE03B8634.  M6II accepts 48000/8000 Hz, 8/16/24-bit, 1/2/4 channels.
 *
 * The native AudioRec callback receives Canon's completed-buffer event:
 *   +0x04 total bytes
 *   +0xD4 first span address
 *   +0xD8 first span length
 *   +0xDC wrapped/second span address
 *
 * No hardware audio registers are written here.
 */

#include <module.h>
#include <dryos.h>
#include <string.h>
#include <stdint.h>

typedef int (*m6ii_sound_setup_fn)(void *cfg);
typedef int (*m6ii_sound_simple_fn)(void);

#define M6II_SOUND_SETUP ((m6ii_sound_setup_fn)(0xE03B884Eu | 1u))
#define M6II_SOUND_START ((m6ii_sound_simple_fn)(0xE03B8932u | 1u))
#define M6II_SOUND_STOP  ((m6ii_sound_simple_fn)(0xE03B894Eu | 1u))

struct m6ii_sound_stream_cfg
{
    uint32_t unknown_00;
    void *work_start;
    void *work_end;
    uint32_t sample_rate;
    uint32_t bits_per_sample;
    uint32_t channels;
    void (*record_cbr)(void *ctx, void *event);
    void (*stop_cbr)(void *ctx, void *event);
    void *ctx;
};

static struct m6ii_sound_stream_cfg m6ii_cfg;
static volatile int m6ii_configured = 0;
static volatile int m6ii_running = 0;

static uint8_t * volatile legacy_current = 0;
static uint32_t volatile legacy_current_size = 0;
static uint32_t volatile legacy_current_pos = 0;
static uint8_t * volatile legacy_next = 0;
static uint32_t volatile legacy_next_size = 0;
static void (* volatile legacy_complete_cbr)(void) = 0;

static void m6ii_advance_legacy_buffer(void)
{
    legacy_current = legacy_next;
    legacy_current_size = legacy_next_size;
    legacy_current_pos = 0;
    legacy_next = 0;
    legacy_next_size = 0;

    /*
     * Legacy ASIF fires after switching to its already-queued next buffer.
     * mlv_snd's callback then queues another buffer through
     * SetNextASIFADCBuffer(), which fills legacy_next above.
     */
    if (legacy_complete_cbr)
        legacy_complete_cbr();
}

static void m6ii_copy_pcm_span(const uint8_t *src, uint32_t length)
{
    while (src && length && m6ii_running)
    {
        if (!legacy_current || !legacy_current_size)
            return;

        uint32_t room = legacy_current_size - legacy_current_pos;
        uint32_t n = (length < room) ? length : room;

        memcpy(legacy_current + legacy_current_pos, src, n);
        legacy_current_pos += n;
        src += n;
        length -= n;

        if (legacy_current_pos == legacy_current_size)
            m6ii_advance_legacy_buffer();
    }
}

static void m6ii_sound_record_cbr(void *ctx, void *event)
{
    (void)ctx;

    if (!m6ii_running || !event)
        return;

    const uint8_t *ev = (const uint8_t *)event;
    uint32_t total = *(const uint32_t *)(ev + 0x04);
    const uint8_t *first = *(const uint8_t * const *)(ev + 0xD4);
    uint32_t first_len = *(const uint32_t *)(ev + 0xD8);
    const uint8_t *second = *(const uint8_t * const *)(ev + 0xDC);

    if (first_len > total)
        first_len = total;

    m6ii_copy_pcm_span(first, first_len);

    uint32_t second_len = total - first_len;
    if (second_len)
        m6ii_copy_pcm_span(second, second_len);
}

static void m6ii_sound_stop_cbr(void *ctx, void *event)
{
    (void)ctx;
    (void)event;
}

int StartASIFDMAADC(void *buffer1, uint32_t size1,
                    void *buffer2, uint32_t size2,
                    void (*cbr)(void), uint32_t cbr_ctx)
{
    (void)cbr_ctx;

    if (!is_camera("M6II", "1.1.1"))
        return -1;

    if (!buffer1 || !size1 || !buffer2 || !size2 || !cbr)
        return -1;

    legacy_current = (uint8_t *)buffer1;
    legacy_current_size = size1;
    legacy_current_pos = 0;
    legacy_next = (uint8_t *)buffer2;
    legacy_next_size = size2;
    legacy_complete_cbr = cbr;

    if (!m6ii_configured)
    {
        memset(&m6ii_cfg, 0, sizeof(m6ii_cfg));
        m6ii_cfg.sample_rate = 48000;
        m6ii_cfg.bits_per_sample = 16;
        m6ii_cfg.channels = 2;
        m6ii_cfg.record_cbr = m6ii_sound_record_cbr;
        m6ii_cfg.stop_cbr = m6ii_sound_stop_cbr;
        m6ii_cfg.ctx = 0;

        int rc = M6II_SOUND_SETUP(&m6ii_cfg);
        if (rc)
            return rc;

        m6ii_configured = 1;
    }

    m6ii_running = 1;
    int rc = M6II_SOUND_START();
    if (rc)
        m6ii_running = 0;

    return rc;
}

int SetNextASIFADCBuffer(void *buffer, uint32_t size)
{
    if (!is_camera("M6II", "1.1.1"))
        return -1;

    legacy_next = (uint8_t *)buffer;
    legacy_next_size = size;
    return 0;
}

int StopASIFDMAADC(void)
{
    if (!is_camera("M6II", "1.1.1"))
        return -1;

    if (!m6ii_running)
        return 0;

    int rc = M6II_SOUND_STOP();
    m6ii_running = 0;

    legacy_current = 0;
    legacy_current_size = 0;
    legacy_current_pos = 0;
    legacy_next = 0;
    legacy_next_size = 0;
    legacy_complete_cbr = 0;

    return rc;
}

/*
 * mlv_snd references SetSamplingRate as a required legacy symbol.
 * On M6II the native SoundDev stream receives the format in its setup block,
 * so there is no separate legacy sampling-rate call.
 */
void SetSamplingRate(int sample_rate, int channels)
{
    (void)sample_rate;
    (void)channels;
}
