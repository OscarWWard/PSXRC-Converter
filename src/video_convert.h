#ifndef PSXRC_VIDEO_CONVERT_H
#define PSXRC_VIDEO_CONVERT_H

#include "codec.h"
#include "str.h"
#include "writer.h"

typedef struct {
    str_header header;
    uint32_t lba[STR_MAX_CHUNKS];
    uint8_t *original, *rebuilt;
    size_t rebuilt_size;
    unsigned quant;
    bs_mdec_fingerprint mdec;
    AVPacket *packet;
} video_frame;

void video_frame_clear(video_frame *frame);
int video_options_check(const video_options *options);
int video_convert(video_frame *frames, unsigned count, const convert_options *options);

#endif
