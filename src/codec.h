#ifndef PSXRC_CODEC_H
#define PSXRC_CODEC_H

#include "bs.h"
#include <libavcodec/avcodec.h>

#define PSXRC_GROUP_FRAMES 4
#define PSXRC_MAX_PACKET (256u * 1024u)

AVCodecContext *codec_decoder(enum AVCodecID id, unsigned width, unsigned height);
int codec_decode(AVCodecContext *decoder, const uint8_t *data, size_t bytes, AVFrame *frame);
int codec_picture(const AVFrame *frame, unsigned width, unsigned height, bs_picture *picture);

#endif
