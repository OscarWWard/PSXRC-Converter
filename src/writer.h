#ifndef PSXRC_WRITER_H
#define PSXRC_WRITER_H

#include "source.h"

typedef struct { uint32_t first, count; } lba_range;

enum video_rate_control { VIDEO_CRF, VIDEO_QP, VIDEO_ABR };

typedef struct {
    const char *preset, *tune, *profile, *level, *extra;
    enum video_rate_control rate_control;
    unsigned quality, bitrate, maxrate, buffer, references;
} video_options;

typedef struct {
    uint32_t first, count;
    video_options movie;
    unsigned mse_limit, zstd_level, min_saving, mdec_hints;
    unsigned auto_video, audio_bitrate, audio_ratio, cdda, xa, kept;
    lba_range keep[32];
} convert_options;

int keep_sector(const disc_source *source, const convert_options *options, uint32_t lba);
int write_image(disc_source *source, const char *path, const convert_options *options);

#endif
