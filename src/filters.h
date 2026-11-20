#ifndef PSXRC_FILTERS_H
#define PSXRC_FILTERS_H

#include <stdint.h>

void filter_hunk(uint8_t *output, const uint8_t *source, unsigned count, unsigned stride,
                 unsigned cdda);
int filter_masks_valid(const uint8_t *data, unsigned count);
int unfilter_sector(uint8_t *hunk, unsigned index, unsigned stride);

#endif
