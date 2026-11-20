#ifndef PSXRC_AUDIO_H
#define PSXRC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_MAX_UNITS 36
#define AUDIO_MAX_PACKET 1275
#define AUDIO_MAX_PACKETS 32
#define AUDIO_HEADER 12
#define AUDIO_MAX_GROUP (AUDIO_HEADER + AUDIO_MAX_PACKETS * (AUDIO_MAX_PACKET + 2))
#define AUDIO_UNIT_BYTES 2352

enum audio_kind { AUDIO_CDDA = 1, AUDIO_XA4 = 2 };

unsigned audio_rate(enum audio_kind kind, unsigned coding);
unsigned audio_channels(enum audio_kind kind, unsigned coding);
unsigned audio_samples_per_unit(enum audio_kind kind, unsigned coding);
unsigned audio_units_per_group(enum audio_kind kind, unsigned coding);

int audio_unpack(const uint8_t *group, size_t bytes, uint8_t *sectors,
                 enum audio_kind *kind, unsigned *coding, unsigned *units);
size_t audio_pack(enum audio_kind kind, unsigned coding, const int16_t *pcm_with_preroll,
                  unsigned units, unsigned bitrate, unsigned noise_ratio,
                  uint8_t *output);

#endif
