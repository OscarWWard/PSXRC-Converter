#include "audio.h"
#include "xa.h"
#include "bytes.h"
#include <libavutil/adler32.h>
#include <opus.h>
#include <stdlib.h>
#include <string.h>

enum { OPUS_RATE = 48000, OPUS_FRAME = 960, PREROLL_MS = 80 };

unsigned audio_rate(enum audio_kind kind, unsigned coding)
{
    return kind == AUDIO_CDDA ? 44100 : (coding & 4 ? 18900 : 37800);
}

unsigned audio_channels(enum audio_kind kind, unsigned coding)
{
    return kind == AUDIO_CDDA ? 2 : (coding & 1) + 1;
}

unsigned audio_samples_per_unit(enum audio_kind kind, unsigned coding)
{
    return kind == AUDIO_CDDA ? 588 : XA_MAX_SAMPLES / audio_channels(kind, coding);
}

unsigned audio_units_per_group(enum audio_kind kind, unsigned coding)
{
    return audio_rate(kind, coding) * 48 / 100 / audio_samples_per_unit(kind, coding);
}

static void resample(const int16_t *source, unsigned src_length, unsigned src_rate,
                     int16_t *output, unsigned out_length, unsigned out_rate, unsigned channels,
                     unsigned cubic)
{
    unsigned position = 0, fraction = 0;
    for (unsigned i = 0; i < out_length; ++i) {
        unsigned current = position < src_length ? position : src_length - 1;
        unsigned next = position + 1 < src_length ? position + 1 : current;
        unsigned previous = current ? current - 1 : current;
        unsigned following = next + 1 < src_length ? next + 1 : next;
        for (unsigned c = 0; c < channels; ++c) {
            int32_t b = source[current * channels + c], d = source[following * channels + c];
            int32_t a = source[previous * channels + c], next_value = source[next * channels + c];
            int32_t value;
            if (cubic) {
                int32_t t = (int32_t)(fraction * 32768u / out_rate);
                int64_t high = -a + 3 * b - 3 * next_value + d;
                high = high * t / 32768 + 2 * a - 5 * b + 4 * next_value - d;
                high = high * t / 32768 - a + next_value;
                value = b + (int32_t)(high * t / 65536);
            } else {
                value = (b * (int32_t)(out_rate - fraction) +
                         next_value * (int32_t)fraction) / (int32_t)out_rate;
            }
            if (value < -32768)
                value = -32768;
            if (value > 32767)
                value = 32767;
            output[i * channels + c] = (int16_t)value;
        }
        fraction += src_rate;
        position += fraction / out_rate;
        fraction %= out_rate;
    }
}

static int decode_pcm(const uint8_t *p, size_t bytes, unsigned packets, unsigned discard,
                      unsigned rate, unsigned channels, int16_t *pcm, unsigned native_samples,
                      unsigned cubic)
{
    if (!packets || packets > AUDIO_MAX_PACKETS || discard < 3840 || discard > 4800)
        return 0;
    unsigned decoded = packets * OPUS_FRAME;
    if (discard >= decoded || (uint64_t)(decoded - discard) * rate < (uint64_t)native_samples * OPUS_RATE)
        return 0;
    int16_t *buffer = malloc(decoded * channels * sizeof(*buffer));
    if (!buffer)
        return 0;
    int error;
    OpusDecoder *dec = opus_decoder_create(OPUS_RATE, (int)channels, &error);
    int ok = dec && error == OPUS_OK;
    for (unsigned i = 0; ok && i < packets; ++i) {
        if (bytes < 2) {
            ok = 0;
            break;
        }
        unsigned n = read16(p);
        p += 2;
        bytes -= 2;
        if (!n || n > AUDIO_MAX_PACKET || n > bytes ||
            opus_decode(dec, p, (opus_int32)n, buffer + i * OPUS_FRAME * channels, OPUS_FRAME, 0) != OPUS_FRAME) {
            ok = 0;
            break;
        }
        p += n;
        bytes -= n;
    }
    if (ok && !bytes)
        resample(buffer + discard * channels, decoded - discard, OPUS_RATE, pcm, native_samples, rate,
                 channels, cubic);
    if (dec)
        opus_decoder_destroy(dec);
    free(buffer);
    return ok && !bytes;
}

static int rebuild(enum audio_kind kind, unsigned channels, const int16_t *pcm,
                   unsigned samples, unsigned units, uint8_t *sectors)
{
    for (unsigned i = 0; i < units; ++i) {
        uint8_t *sector = sectors + i * AUDIO_UNIT_BYTES;
        if (kind == AUDIO_XA4) {
            if (!xa_encode(pcm, channels, sector))
                return 0;
            memset(sector + XA_DATA_BYTES, 0, AUDIO_UNIT_BYTES - XA_DATA_BYTES);
        } else {
            for (unsigned s = 0; s < samples * channels; ++s)
                write16(sector + s * 2, (uint16_t)pcm[s]);
        }
        pcm += samples * channels;
    }
    return 1;
}

int audio_unpack(const uint8_t *group, size_t bytes, uint8_t *sectors,
                 enum audio_kind *kind, unsigned *coding, unsigned *units)
{
    if (bytes < AUDIO_HEADER)
        return 0;
    *kind = (enum audio_kind)group[0];
    *coding = group[1];
    *units = group[2];
    if ((*kind != AUDIO_CDDA && *kind != AUDIO_XA4) || (*coding & ~5u) ||
        (*kind == AUDIO_CDDA && *coding) || !*units || *units > audio_units_per_group(*kind, *coding) ||
        read16(group + 6) > 1)
        return 0;
    unsigned samples = audio_samples_per_unit(*kind, *coding);
    unsigned channels = audio_channels(*kind, *coding);
    int16_t *pcm = malloc(*units * samples * channels * sizeof(*pcm));
    if (!pcm)
        return 0;
    int ok = decode_pcm(group + AUDIO_HEADER, bytes - AUDIO_HEADER, group[3], read16(group + 4),
                        audio_rate(*kind, *coding), channels, pcm, *units * samples, read16(group + 6)) &&
             rebuild(*kind, channels, pcm, samples, *units, sectors) &&
             av_adler32_update(1, sectors, *units * AUDIO_UNIT_BYTES) == read32(group + 8);
    free(pcm);
    return ok;
}

static int quality_passes(const int16_t *original, const int16_t *rebuilt, unsigned samples,
                          unsigned ratio)
{
    uint64_t noise = 0, signal = 0;
    for (unsigned i = 0; i < samples; ++i) {
        int64_t difference = original[i] - rebuilt[i];
        noise += (uint64_t)(difference * difference);
        signal += (uint64_t)((int32_t)original[i] * original[i]);
    }
    return noise <= signal / ratio + (uint64_t)samples * 64 * 64;
}

size_t audio_pack(enum audio_kind kind, unsigned coding, const int16_t *source,
                  unsigned units, unsigned bitrate, unsigned noise_ratio, uint8_t *out)
{
    unsigned rate = audio_rate(kind, coding), channels = audio_channels(kind, coding);
    unsigned preroll = rate * PREROLL_MS / 1000;
    unsigned samples = audio_samples_per_unit(kind, coding);
    unsigned native = samples * units;
    unsigned up = ((native + preroll) * OPUS_RATE + rate - 1) / rate;
    int error, lookahead;
    OpusEncoder *enc = opus_encoder_create(OPUS_RATE, (int)channels, OPUS_APPLICATION_AUDIO, &error);
    if (!enc)
        return 0;
    size_t result = 0;
    if (error != OPUS_OK || opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead < 0 || lookahead > 960 ||
        opus_encoder_ctl(enc, OPUS_SET_BITRATE((opus_int32)bitrate)) != OPUS_OK)
        goto done;
    unsigned packets = (up + (unsigned)lookahead + OPUS_FRAME - 1) / OPUS_FRAME;
    if (packets > AUDIO_MAX_PACKETS)
        goto done;
    int16_t *buffer = calloc(packets * OPUS_FRAME * channels, sizeof(*buffer));
    int16_t *rebuilt = malloc(native * channels * sizeof(*rebuilt));
    uint8_t *sectors = malloc(units * AUDIO_UNIT_BYTES);
    if (!buffer || !rebuilt || !sectors)
        goto finish;
    resample(source, native + preroll, rate, buffer, up, OPUS_RATE, channels, 1);
    unsigned discard = OPUS_RATE * PREROLL_MS / 1000 + (unsigned)lookahead;
    uint8_t *p = out + AUDIO_HEADER;
    for (unsigned i = 0; i < packets; ++i) {
        opus_int32 n = opus_encode(enc, buffer + i * OPUS_FRAME * channels, OPUS_FRAME, p + 2, AUDIO_MAX_PACKET);
        if (n <= 0)
            goto finish;
        write16(p, (unsigned)n);
        p += n + 2;
    }
    size_t length = (size_t)(p - out);
    if (!decode_pcm(out + AUDIO_HEADER, length - AUDIO_HEADER, packets, discard, rate, channels, rebuilt, native, 1) ||
        !rebuild(kind, channels, rebuilt, samples, units, sectors))
        goto finish;
    if (kind == AUDIO_XA4) {
        for (unsigned i = 0; i < units; ++i) {
            xa_predictors state = { 0 };
            if (!xa_decode(sectors + i * AUDIO_UNIT_BYTES, channels, &state, rebuilt + i * samples * channels))
                goto finish;
        }
    }
    if (!quality_passes(source + preroll * channels, rebuilt, native * channels, noise_ratio))
        goto finish;
    out[0] = (uint8_t)kind;
    out[1] = (uint8_t)coding;
    out[2] = (uint8_t)units;
    out[3] = (uint8_t)packets;
    write16(out + 4, discard);
    write16(out + 6, 1);
    write32(out + 8, av_adler32_update(1, sectors, units * AUDIO_UNIT_BYTES));
    result = length;
finish:
    free(buffer);
    free(rebuilt);
    free(sectors);
done:
    opus_encoder_destroy(enc);
    return result;
}
