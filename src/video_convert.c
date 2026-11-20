#include "video_convert.h"
#include <libavutil/log.h>
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

static int safe_extra_parameters(const char *extra)
{
    static const char *const names[] = {
        "aq-mode", "aq-strength", "psy", "psy-rd", "deblock", "no-deblock",
        "me", "merange", "subme", "trellis", "partitions", "weightp", "8x8dct",
        "deadzone-inter", "deadzone-intra", "nr", "mixed-refs", "cabac",
        "fast-pskip", "chroma-me", "chroma-qp-offset", "qcomp", "qpmin", "qpmax",
        "qpstep", "ipratio", "slices", "slice-max-size",
    };
    if (strlen(extra) > 1024)
        return 0;
    while (*extra) {
        const char *end = strchr(extra, ':');
        if (!end)
            end = extra + strlen(extra);
        const char *value = memchr(extra, '=', (size_t)(end - extra));
        if (!value || value + 1 == end)
            return 0;
        int known = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (strlen(names[i]) == (size_t)(value - extra) &&
                !memcmp(names[i], extra, (size_t)(value - extra)))
                known = 1;
        if (!known)
            return 0;
        for (const char *p = value + 1; p < end; ++p)
            if (*p <= ' ' || *p >= 0x7f || *p == '"' || *p == '\\' || *p == '=')
                return 0;
        if (*end && !end[1])
            return 0;
        extra = *end ? end + 1 : end;
    }
    return 1;
}

static AVCodecContext *video_encoder(unsigned width, unsigned height, const video_options *options)
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
    if (options->references)
        encoder->refs = (int)options->references;
    encoder->rc_max_rate = options->maxrate * 1000;
    encoder->rc_buffer_size = options->buffer * 1000;
    char tune[48], params[1200];
    snprintf(tune, sizeof(tune), "%s%szerolatency", options->tune, *options->tune ? "," : "");
    snprintf(params, sizeof(params), "%s%sopen-gop=0:scenecut=0:repeat-headers=1:annexb=1:bframes=0",
             options->extra, *options->extra ? ":" : "");
    int rate_ok = 1;
    if (options->rate_control == VIDEO_ABR)
        encoder->bit_rate = options->bitrate * 1000;
    else
        rate_ok = av_opt_set_int(encoder->priv_data,
                   options->rate_control == VIDEO_QP ? "qp" : "crf", options->quality, 0) >= 0;
    if (!rate_ok || av_opt_set(encoder->priv_data, "preset", options->preset, 0) < 0 ||
        av_opt_set(encoder->priv_data, "tune", tune, 0) < 0 ||
        av_opt_set(encoder->priv_data, "profile", options->profile, 0) < 0 ||
        (*options->level && av_opt_set(encoder->priv_data, "level", options->level, 0) < 0) ||
        av_opt_set(encoder->priv_data, "x264-params", params, 0) < 0 ||
        avcodec_open2(encoder, codec, NULL) < 0)
        avcodec_free_context(&encoder);
    return encoder;
}

static int invalid_parameter;

static void validation_log(void *context, int level, const char *format, va_list args)
{
    /* libavcodec reports unknown or malformed x264-params as warnings and keeps going. */
    if (level == AV_LOG_WARNING && strstr(format, "Error parsing option")) {
        invalid_parameter = 1;
        level = AV_LOG_ERROR;
    }
    av_log_default_callback(context, level, format, args);
}

int video_options_check(const video_options *options)
{
    if (!safe_extra_parameters(options->extra) ||
        (!!options->maxrate != !!options->buffer) ||
        (options->maxrate && options->rate_control == VIDEO_QP) ||
        (options->maxrate && options->rate_control == VIDEO_ABR && options->bitrate > options->maxrate)) {
        fputs("Invalid H.264 settings. Check the advanced options and provide both VBV values when using VBV.\n",
              stderr);
        return 0;
    }
    invalid_parameter = 0;
    av_log_set_callback(validation_log);
    AVCodecContext *encoder = video_encoder(16, 16, options);
    av_log_set_callback(av_log_default_callback);
    int valid = encoder && !invalid_parameter;
    avcodec_free_context(&encoder);
    if (!valid) {
        fputs("Could not initialize the H.264 encoder with these settings.\n", stderr);
        return 0;
    }
    return 1;
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

int video_convert(video_frame *frames, unsigned count, const convert_options *options)
{
    if (!count || count > PSXRC_GROUP_FRAMES)
        return 0;
    unsigned width = frames[0].header.width, height = frames[0].header.height;
    AVCodecContext *native = codec_decoder(AV_CODEC_ID_MDEC, width, height);
    AVCodecContext *playback = codec_decoder(AV_CODEC_ID_H264, width, height);
    AVCodecContext *encoder = video_encoder(width, height, &options->movie);
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
            frame->rebuilt_size = bs_encode_fingerprint(&decoded_pixels, h->version, quant, h->words,
                                      frame->rebuilt, h->length, options->mdec_hints ? &frame->mdec : NULL);
            if (frame->rebuilt_size || quant == 63) {
                frame->quant = quant;
                break;
            }
        }
        if (!frame->rebuilt_size ||
            !codec_decode(native, frame->rebuilt, frame->rebuilt_size, rebuilt) ||
            !codec_picture(rebuilt, width, height, &rebuilt_pixels) ||
            !quality_passes(&source_pixels, &rebuilt_pixels, options->mse_limit))
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
