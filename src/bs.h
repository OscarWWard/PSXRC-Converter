#ifndef PSXRC_BS_H
#define PSXRC_BS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *plane[3];  /* Y, Cb, Cr, full range */
    ptrdiff_t stride[3];
    unsigned width, height;
} bs_picture;

/* Returns the padded BS length, or zero if either budget cannot be met. */
size_t bs_encode(const bs_picture *picture, unsigned version, unsigned quant,
                 unsigned word_limit, uint8_t *output, size_t capacity);

#endif
