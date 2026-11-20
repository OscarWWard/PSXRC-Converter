#include "video_convert.h"
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

void video_frame_clear(video_frame *frame)
{
    free(frame->original);
    free(frame->rebuilt);
    av_packet_free(&frame->packet);
    memset(frame, 0, sizeof(*frame));
}

static AVCodecContext *video_encoder(unsigned width, unsigned height, unsigned crf)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        return NULL;
    AVCodecContext *encoder = avcodec_alloc_context3(codec);
    if (!encoder)
        return NULL;
    encoder->time_base = (AVRational){ 1, 15 };
    encoder->framerate = (AVRational){ 15, 1 };
    encoder->width = (int)width;
    encoder->height = (int)height;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->color_range = AVCOL_RANGE_JPEG;
    encoder->gop_size = PSXRC_GROUP_FRAMES;
    encoder->max_b_frames = 0;
    encoder->thread_count = 1;
    if (av_opt_set(encoder->priv_data, "preset", "medium", 0) < 0 ||
        av_opt_set(encoder->priv_data, "tune", "zerolatency", 0) < 0 ||
        av_opt_set(encoder->priv_data, "profile", "high", 0) < 0 ||
        av_opt_set(encoder->priv_data, "x264-params", "open-gop=0:scenecut=0:repeat-headers=1", 0) < 0 ||
        av_opt_set_int(encoder->priv_data, "crf", crf, 0) < 0 ||
        avcodec_open2(encoder, codec, NULL) < 0)
        avcodec_free_context(&encoder);
    return encoder;
}

static int quality_passes(const bs_picture *source, const bs_picture *rebuilt, unsigned limit)
{
    for (unsigned plane = 0; plane < 3; ++plane) {
        unsigned width = source->width >> (plane != 0);
        unsigned height = source->height >> (plane != 0);
        uint64_t squared_error = 0;
        for (unsigned y = 0; y < height; ++y) {
            const uint8_t *a = source->plane[plane] + y * source->stride[plane];
            const uint8_t *b = rebuilt->plane[plane] + y * rebuilt->stride[plane];
            for (unsigned x = 0; x < width; ++x) {
                int diff = a[x] - b[x];
                squared_error += (unsigned)(diff * diff);
            }
        }
        if (squared_error > (uint64_t)limit * width * height)
            return 0;
    }
    return 1;
}

int video_convert(video_frame *frames, unsigned count, unsigned crf, unsigned mse_limit)
{
    if (!count || count > PSXRC_GROUP_FRAMES)
        return 0;
    unsigned width = frames[0].header.width, height = frames[0].header.height;
    AVCodecContext *native = codec_decoder(AV_CODEC_ID_MDEC, width, height);
    AVCodecContext *playback = codec_decoder(AV_CODEC_ID_H264, width, height);
    AVCodecContext *encoder = video_encoder(width, height, crf);
    AVFrame *original = av_frame_alloc();
    AVFrame *decoded = av_frame_alloc();
    AVFrame *rebuilt = av_frame_alloc();
    int result = 0;
    if (!native || !playback || !encoder || !original || !decoded || !rebuilt)
        goto done;
    for (unsigned i = 0; i < count; ++i) {
        video_frame *frame = &frames[i];
        str_header *h = &frame->header;
        bs_picture source_pixels, decoded_pixels, rebuilt_pixels;
        frame->packet = av_packet_alloc();
        frame->rebuilt = malloc(h->length);
        if (!frame->packet || !frame->rebuilt || h->width != width || h->height != height ||
            !codec_decode(native, frame->original, h->length, original) ||
            !codec_picture(original, width, height, &source_pixels))
            goto done;
        original->format = encoder->pix_fmt;
        original->pts = i;
        original->pict_type = AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(encoder, original) < 0 ||
            avcodec_receive_packet(encoder, frame->packet) < 0 ||
            (i == 0 && !(frame->packet->flags & AV_PKT_FLAG_KEY)) ||
            !codec_decode(playback, frame->packet->data, (size_t)frame->packet->size, decoded) ||
            !codec_picture(decoded, width, height, &decoded_pixels))
            goto done;

        unsigned step = h->quant / 4 + 1;
        for (unsigned quant = h->quant; ; quant += step) {
            if (quant > 63)
                quant = 63;
            frame->rebuilt_size = bs_encode(&decoded_pixels, h->version, quant, h->words,
                                            frame->rebuilt, h->length);
            if (frame->rebuilt_size || quant == 63) {
                frame->quant = quant;
                break;
            }
        }
        if (!frame->rebuilt_size ||
            !codec_decode(native, frame->rebuilt, frame->rebuilt_size, rebuilt) ||
            !codec_picture(rebuilt, width, height, &rebuilt_pixels) ||
            !quality_passes(&source_pixels, &rebuilt_pixels, mse_limit))
            goto done;
    }
    result = 1;
done:
    av_frame_free(&original);
    av_frame_free(&decoded);
    av_frame_free(&rebuilt);
    avcodec_free_context(&native);
    avcodec_free_context(&encoder);
    avcodec_free_context(&playback);
    return result;
}
