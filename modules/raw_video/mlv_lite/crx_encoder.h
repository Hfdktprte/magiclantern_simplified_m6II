#ifndef CRX_ENCODER_H
#define CRX_ENCODER_H

#include <stdint.h>

int crx_encode_frame(
    void *source, uint32_t source_width, uint32_t source_height,
    uint32_t source_pitch,
    uint32_t crop_x, uint32_t crop_y, uint32_t width, uint32_t height,
    void *destination, uint32_t destination_size,
    uint32_t *compressed_size);

#endif
