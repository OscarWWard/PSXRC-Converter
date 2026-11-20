#include "codec.h"
#include <string.h>

AVCodecContext *codec_decoder(enum AVCodecID id, unsigned width, unsigned height)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec)
        return NULL;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context)
        return NULL;
    context->width = (int)width;
    context->height = (int)height;
    context->max_pixels = 1050624;
    context->thread_count = 1;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->err_recognition = AV_EF_EXPLODE;
    if (avcodec_open2(context, codec, NULL) < 0)
        avcodec_free_context(&context);
    return context;
}

int codec_decode(AVCodecContext *decoder, const uint8_t *data, size_t bytes, AVFrame *frame)
{
    if (!decoder || !frame || !bytes || bytes > PSXRC_MAX_PACKET)
        return 0;
    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return 0;
    int result = av_new_packet(packet, (int)bytes);
    if (result >= 0) {
        memcpy(packet->data, data, bytes);
        result = avcodec_send_packet(decoder, packet);
    }
    av_packet_free(&packet);
    av_frame_unref(frame);
    return result >= 0 && avcodec_receive_frame(decoder, frame) >= 0 &&
           !(frame->flags & AV_FRAME_FLAG_CORRUPT);
}

int codec_picture(const AVFrame *frame, unsigned width, unsigned height, bs_picture *picture)
{
    if (frame->width != (int)width || frame->height != (int)height ||
        (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) ||
        frame->color_range != AVCOL_RANGE_JPEG)
        return 0;
    picture->width = width;
    picture->height = height;
    for (unsigned i = 0; i < 3; ++i) {
        picture->plane[i] = frame->data[i];
        picture->stride[i] = frame->linesize[i];
    }
    return 1;
}
