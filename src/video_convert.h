#ifndef PSXRC_VIDEO_CONVERT_H
#define PSXRC_VIDEO_CONVERT_H

#include "codec.h"
#include "str.h"

typedef struct {
    str_header header;
    uint32_t lba[STR_MAX_CHUNKS];
    uint8_t *original, *rebuilt;
    size_t rebuilt_size;
    unsigned quant;
    AVPacket *packet;
} video_frame;

void video_frame_clear(video_frame *frame);
int video_convert(video_frame *frames, unsigned count, unsigned crf, unsigned mse_limit);

#endif
