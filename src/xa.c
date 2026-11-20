/* 4-bit CD-XA sector layout and predictor coefficients:
 * https://psx-spx.consoledev.net/dev/ps1/cdr/cdromformat/
 */
#include "xa.h"
#include "str.h"
#include "bytes.h"
#include <string.h>

static const int coefficients[4][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}};
static const uint8_t header_index[] = {0, 1, 2, 3, 8, 9, 10, 11};

static int predict(const int *history, unsigned filter)
{
    int n = coefficients[filter][0] * history[0] + coefficients[filter][1] * history[1];
    return n >= 0 ? n / 64 : (n - 63) / 64;
}

static void advance(int *history, int sample)
{
    history[1] = history[0];
    history[0] = sample;
}

int xa_header(const uint8_t *sector)
{
    static const uint8_t sync[] = {
        0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0
    };
    if (memcmp(sector, sync, sizeof(sync)) || sector[15] != 2 ||
        memcmp(sector + 16, sector + 20, 4) || sector[17] >= 32 ||
        (sector[18] & 0x7e) != 0x64 || (sector[19] & ~5u))
        return 0;
    uint32_t edc = read32(sector + 2348);
    return !edc || edc == str_edc(sector + 16, 2332);
}

int xa_decode(const uint8_t *data, unsigned channels, xa_predictors *state, int16_t *output)
{
    for (unsigned group = 0; group < 18; ++group) {
        const uint8_t *p = data + group * 128;
        if (memcmp(p, p + 4, 4) || memcmp(p + 8, p + 12, 4))
            return 0;
        for (unsigned unit = 0; unit < 8; ++unit) {
            unsigned channel = unit % channels;
            unsigned filter = p[header_index[unit]] >> 4;
            unsigned range = p[header_index[unit]] & 15;
            if (filter > 3 || range > 12)
                return 0;
            int *history = state->history[channel];
            for (unsigned i = 0; i < 28; ++i) {
                int nibble = (p[16 + i * 4 + unit / 2] >> ((unit % 2) * 4)) & 15;
                int sample = (nibble < 8 ? nibble : nibble - 16) * (1 << (12 - range)) +
                             predict(history, filter);
                if (sample < -32768 || sample > 32767)
                    return 0;
                output[(group * 8 + unit - channel) * 28 + i * channels + channel] = (int16_t)sample;
                advance(history, sample);
            }
        }
    }
    return 1;
}

int xa_encode(const int16_t *samples, unsigned channels, uint8_t *output)
{
    xa_predictors state = { 0 };
    memset(output, 0, XA_DATA_BYTES);
    for (unsigned group = 0; group < 18; ++group) {
        uint8_t *p = output + group * 128;
        for (unsigned unit = 0; unit < 8; ++unit) {
            unsigned channel = unit % channels;
            const int16_t *input = samples + (group * 8 + unit - channel) * 28 + channel;
            int *current = state.history[channel];
            unsigned filters = group || unit >= channels ? 4 : 1;
            uint64_t best_score = UINT64_MAX;
            int best_history[2] = { 0 };
            unsigned best_parameter = 0;
            uint8_t best_nibbles[28] = { 0 };
            for (unsigned filter = 0; filter < filters; ++filter) {
                int peak = 0;
                int predicted[2] = { current[0], current[1] };
                for (unsigned i = 0; i < 28; ++i) {
                    int sample = input[i * channels];
                    int diff = sample - predict(predicted, filter);
                    if (diff < 0)
                        diff = -diff;
                    if (diff > peak)
                        peak = diff;
                    advance(predicted, sample);
                }
                unsigned shift = 0;
                while (shift < 12 && peak > (7 << shift))
                    ++shift;
                int step = 1 << shift;
                int history[2] = { current[0], current[1] };
                uint8_t nibbles[28];
                uint64_t score = 0;
                for (unsigned i = 0; i < 28; ++i) {
                    int goal = input[i * channels];
                    int prediction = predict(history, filter);
                    int residual = goal - prediction;
                    int q = residual < 0 ? -((-residual + step / 2) / step) : (residual + step / 2) / step;
                    if (q < -8)
                        q = -8;
                    if (q > 7)
                        q = 7;
                    while (prediction + q * step < -32768 && q < 7)
                        ++q;
                    while (prediction + q * step > 32767 && q > -8)
                        --q;
                    int value = prediction + q * step;
                    if (value < -32768 || value > 32767) {
                        score = UINT64_MAX;
                        break;
                    }
                    int64_t diff = goal - value;
                    score += (uint64_t)(diff * diff);
                    nibbles[i] = (uint8_t)(q & 15);
                    advance(history, value);
                }
                if (score < best_score) {
                    best_score = score;
                    best_parameter = filter * 16 + 12 - shift;
                    memcpy(best_nibbles, nibbles, sizeof(nibbles));
                    memcpy(best_history, history, sizeof(history));
                }
            }
            if (best_score == UINT64_MAX)
                return 0;
            memcpy(current, best_history, sizeof(best_history));
            p[header_index[unit]] = p[header_index[unit] + 4] = (uint8_t)best_parameter;
            for (unsigned i = 0; i < 28; ++i)
                p[16 + i * 4 + unit / 2] |= best_nibbles[i] << ((unit % 2) * 4);
        }
    }
    return 1;
}
