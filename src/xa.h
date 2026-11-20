#ifndef PSXRC_XA_H
#define PSXRC_XA_H

#include <stdint.h>

#define XA_DATA_BYTES 2304
#define XA_MAX_SAMPLES 4032

typedef struct { int history[2][2]; } xa_predictors;

int xa_header(const uint8_t *sector);
int xa_decode(const uint8_t *data, unsigned channels, xa_predictors *state, int16_t *output);
int xa_encode(const int16_t *samples, unsigned channels, uint8_t *output);

#endif
