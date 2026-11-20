#include "filters.h"
#include "bytes.h"
#include "format.h"
#include "str.h"
#include <string.h>

enum { ECC_OFFSET = 2072, CDDA_VALUES = PSXRC_SECTOR_BYTES / 2 };

static void split_deltas(uint8_t *out, const uint8_t *source)
{
    uint16_t previous[2] = { 0 };
    for (unsigned i = 0; i < CDDA_VALUES; ++i) {
        uint16_t sample = read16(source + i * 2);
        uint16_t delta = (uint16_t)(sample - previous[i & 1]);
        out[i] = (uint8_t)delta;
        out[i + CDDA_VALUES] = (uint8_t)(delta >> 8);
        previous[i & 1] = sample;
    }
}

static void join_deltas(uint8_t *data)
{
    uint8_t copy[PSXRC_SECTOR_BYTES];
    uint16_t previous[2] = { 0 };
    memcpy(copy, data, sizeof(copy));
    for (unsigned i = 0; i < CDDA_VALUES; ++i) {
        unsigned delta = copy[i] | (unsigned)copy[i + CDDA_VALUES] << 8;
        previous[i & 1] = (uint16_t)(previous[i & 1] + delta);
        write16(data + i * 2, previous[i & 1]);
    }
}

void filter_hunk(uint8_t *output, const uint8_t *source, unsigned count, unsigned stride,
                 unsigned cdda)
{
    unsigned ecc = 0;
    uint8_t *data = output + PSXRC_FILTER_BYTES;
    memcpy(data, source, count * stride);
    for (unsigned i = 0; i < count; ++i) {
        unsigned bit = 1u << i;
        if (cdda & bit) {
            split_deltas(data, source);
        } else if (str_form1_integrity(source)) {
            memset(data + ECC_OFFSET, 0, PSXRC_SECTOR_BYTES - ECC_OFFSET);
            ecc |= bit;
        }
        data += stride;
        source += stride;
    }
    output[0] = (uint8_t)ecc;
    output[1] = (uint8_t)cdda;
}

int unfilter_hunk(uint8_t *data, unsigned count, unsigned stride)
{
    unsigned ecc = data[0], cdda = data[1];
    if (ecc & cdda || ((ecc | cdda) >> count))
        return 0;
    data += PSXRC_FILTER_BYTES;
    for (unsigned i = 0; i < count; ++i, data += stride) {
        unsigned bit = 1u << i;
        if (ecc & bit) {
            if (data[15] != 2 || (data[18] & 0x20))
                return 0;
            str_write_integrity(data);
        } else if (cdda & bit) {
            join_deltas(data);
        }
    }
    return 1;
}
