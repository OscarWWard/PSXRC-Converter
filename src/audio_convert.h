#ifndef PSXRC_AUDIO_CONVERT_H
#define PSXRC_AUDIO_CONVERT_H

#include "writer.h"

typedef struct {
    FILE *data;
    psxrc_record *group;
    uint32_t *slot;
    uint32_t groups, sectors;
    uint64_t size;
    uint8_t *xa;
} audio_plan;

int audio_plan_build(disc_source *source, audio_plan *plan, const convert_options *options);

#endif
