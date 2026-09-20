#ifndef _CRX_D8_H_
#define _CRX_D8_H_

#include <stdint.h>

/*
 * Experimental EOS M6 Mark II / DIGIC 8 CRX hardware encoder backend.
 *
 * This is intentionally narrow: 14-bit Bayer, lossless CRX (DWT level 0),
 * one frame at a time.  The caller supplies a full-resolution source buffer
 * and an MLV frame payload buffer.  The backend packs the requested crop into
 * a contiguous staging area before handing it to Canon's CRX encoder.
 */

int crx_d8_supported(void);
int crx_d8_start_recording(int width, int height);
void crx_d8_stop_recording(void);

/* bytes required after the 64-byte VIDF area for a CRX frame slot */
uint32_t crx_d8_slot_payload_capacity(int width, int height);

/*
 * Encode one cropped frame.
 * Returns exact CRX sample size (subheader + encoded payload), negative on
 * failure.  On success the CRX sample begins at dst.
 */
int crx_d8_compress_raw_rectangle(
    void *dst, uint32_t dst_capacity,
    const void *src, int src_pitch,
    int src_x, int src_y,
    int width, int height
);

/* stream-level CRX main header, generated once after start_recording() */
const void *crx_d8_main_header(uint32_t *size);

#endif
