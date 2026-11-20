/* PS1 BS v2/v3 syntax: https://psx-spx.consoledev.net/dev/ps1/cdr/cdromfileformats/streaming/ */
#include "bs.h"
#include "bytes.h"
#include <string.h>

/* First half of the orthonormal 8-point DCT basis, scaled by 2^14.
   Each row is symmetric or antisymmetric about the center. */
static const int16_t basis[8][4] = {
    { 5793, 5793, 5793, 5793 },
    { 8035, 6811, 4551, 1598 },
    { 7568, 3135,-3135,-7568 },
    { 6811,-1598,-8035,-4551 },
    { 5793,-5793,-5793, 5793 },
    { 4551,-8035, 1598, 6811 },
    { 3135,-7568, 7568,-3135 },
    { 1598,-4551, 6811,-8035 }
};

static const uint8_t zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static const uint8_t quant_matrix[64] = {
     2,16,19,22,26,27,29,34, 16,16,22,24,27,29,34,37,
    19,22,26,27,29,34,34,38, 22,22,26,27,29,34,37,40,
    22,26,27,29,32,35,40,48, 26,27,29,32,35,40,48,58,
    26,27,29,34,38,46,56,69, 27,29,35,38,46,56,69,83
};

/* Positive run/level values in prefix order. The sign follows the prefix. */
static const int16_t ac_codes[111] = {
    0x0001,0x0401,0x0002,0x0801,0x1001,0x0c01,0x0003,
    0x3401,0x0006,0x3001,0x2c01,0x0c02,0x0403,0x0005,0x2801,
    0x1c01,0x1801,0x0402,0x1401,0x0802,0x2401,0x0004,0x2001,
    0x4001,0x1402,0x0007,0x0803,0x0404,0x3c01,0x3801,0x1002,
    0x000b,0x2002,0x1003,0x000a,0x0804,0x1c02,0x5401,0x5001,
    0x0009,0x4c01,0x4801,0x0405,0x0c03,0x0008,0x1802,0x4401,
    0x2802,0x2402,0x1403,0x0c04,0x0805,0x0407,0x0406,0x000f,
    0x000e,0x000d,0x000c,0x6801,0x6401,0x6001,0x5c01,0x5801,
    0x001f,0x001e,0x001d,0x001c,0x001b,0x001a,0x0019,0x0018,
    0x0017,0x0016,0x0015,0x0014,0x0013,0x0012,0x0011,0x0010,
    0x0028,0x0027,0x0026,0x0025,0x0024,0x0023,0x0022,0x0021,
    0x0020,0x040e,0x040d,0x040c,0x040b,0x040a,0x0409,0x0408,
    0x0412,0x0411,0x0410,0x040f,0x1803,0x4002,0x3c02,0x3802,
    0x3402,0x3002,0x2c02,0x7c01,0x7801,0x7401,0x7001,0x6c01
};
static const struct { uint8_t count, bits, first; } ac_groups[] = {
    {1,2,3}, {1,3,3}, {2,4,4}, {2,5,6}, {1,5,5}, {8,8,32},
    {4,6,4}, {4,7,4}, {8,10,8}, {16,12,16}, {16,13,16},
    {16,14,16}, {16,15,16}, {16,16,16}
};

typedef struct {
    struct { uint16_t value; uint8_t bits; } ac[32][41];
    uint8_t *output;
    size_t capacity, size;
    uint32_t pending;
    unsigned bits, codes;
    int dc[3];
    int failed;
} bit_writer;

static int divide_round(int64_t numerator, int denominator)
{
    if (numerator < 0)
        return -(int)((-numerator + denominator / 2) / denominator);
    return (int)((numerator + denominator / 2) / denominator);
}

static void put_bits(bit_writer *w, unsigned value, unsigned count)
{
    if (w->failed)
        return;
    w->pending = (w->pending << count) | value;
    w->bits += count;
    while (w->bits >= 16) {
        w->bits -= 16;
        if (w->size + 2 > w->capacity) {
            w->failed = 1;
            return;
        }
        write16(w->output + w->size, w->pending >> w->bits);
        w->size += 2;
    }
}

static void put_ac(bit_writer *w, unsigned run, int level)
{
    unsigned magnitude = (unsigned)(level < 0 ? -level : level);
    if (run < 32 && magnitude <= 40 && w->ac[run][magnitude].bits) {
        put_bits(w, w->ac[run][magnitude].value * 2 + (level < 0),
                 w->ac[run][magnitude].bits + 1);
        return;
    }
    put_bits(w, 1, 6);
    put_bits(w, run * 1024 + ((unsigned)level & 1023), 16);
}

static void put_dc(bit_writer *w, unsigned version, unsigned plane, int dc)
{
    static const uint8_t luma_code[]  = {4, 0, 1, 5, 6, 14, 30, 62, 126};
    static const uint8_t luma_bits[]  = {3, 2, 2, 3, 3,  4,  5,  6,   7};
    static const uint8_t chroma_code[]= {0, 1, 2, 6,14, 30, 62,126, 254};
    static const uint8_t chroma_bits[]= {2, 2, 2, 3, 4,  5,  6,  7,   8};
    if (version == 2) {
        put_bits(w, (unsigned)dc & 1023, 10);
        return;
    }
    dc = divide_round(dc, 4);
    if (dc > 127)
        dc = 127;
    int delta = dc - w->dc[plane];
    w->dc[plane] = dc;
    unsigned size = 0;
    for (unsigned abs_delta = (unsigned)(delta < 0 ? -delta : delta);
         abs_delta; abs_delta >>= 1)
        ++size;
    put_bits(w, plane ? chroma_code[size] : luma_code[size],
             plane ? chroma_bits[size] : luma_bits[size]);
    if (size)
        put_bits(w, (unsigned)(delta >= 0 ? delta : delta + (1 << size) - 1), size);
}

static void encode_block(bit_writer *w, const bs_picture *picture, unsigned plane,
                         unsigned x0, unsigned y0, unsigned version, unsigned quant)
{
    int32_t columns[8][8];
    unsigned width = (picture->width + (plane != 0)) >> (plane != 0);
    unsigned height = (picture->height + (plane != 0)) >> (plane != 0);
    for (unsigned y = 0; y < 8; ++y) {
        unsigned source_y = y0 + y < height ? y0 + y : height - 1;
        const uint8_t *row = picture->plane[plane] + (ptrdiff_t)source_y * picture->stride[plane];
        int32_t even[4], odd[4];
        for (unsigned x = 0; x < 4; ++x) {
            unsigned left = x0 + x < width ? x0 + x : width - 1;
            unsigned right = x0 + 7 - x < width ? x0 + 7 - x : width - 1;
            even[x] = (int)row[left] + row[right] - 256;
            odd[x] = (int)row[left] - row[right];
        }
        for (unsigned u = 0; u < 8; ++u) {
            const int32_t *values = u & 1 ? odd : even;
            int sum = 0;
            for (unsigned x = 0; x < 4; ++x)
                sum += values[x] * basis[u][x];
            columns[u][y] = sum;
        }
    }
    int32_t vertical[2][8][4];
    for (unsigned u = 0; u < 8; ++u)
        for (unsigned y = 0; y < 4; ++y) {
            vertical[0][u][y] = columns[u][y] + columns[u][7 - y];
            vertical[1][u][y] = columns[u][y] - columns[u][7 - y];
        }
    unsigned run = 0;
    for (unsigned i = 0; i < 64; ++i) {
        unsigned position = zigzag[i];
        unsigned u = position % 8, v = position / 8;
        int64_t sum = 0;
        for (unsigned y = 0; y < 4; ++y)
            sum += (int64_t)vertical[v & 1][u][y] * basis[v][y];
        int coefficient = divide_round(sum, 1 << 28);
        int level;
        if (i) {
            int divisor = (int)quant * quant_matrix[position];
            int magnitude = (coefficient < 0 ? -coefficient : coefficient) * 8 + divisor / 2;
            if (magnitude < divisor) {
                ++run;
                continue;
            }
            level = magnitude / divisor;
            if (coefficient < 0)
                level = -level;
        } else {
            level = divide_round(coefficient, 2);
        }
        if (level < -512)
            level = -512;
        if (level > 510)
            level = 510;
        if (i == 0) {
            put_dc(w, version, plane, level);
            ++w->codes;
        } else if (level) {
            put_ac(w, run, level);
            run = 0;
            ++w->codes;
        } else {
            ++run;
        }
    }
    put_bits(w, 2, 2);
    ++w->codes;
}

size_t bs_encode(const bs_picture *picture, unsigned version, unsigned quant,
                 unsigned word_limit, uint8_t *output, size_t capacity)
{
    if (!picture || !output || !picture->width || !picture->height ||
        picture->width > 1024 || picture->height > 1024 ||
        (version != 2 && version != 3) || !quant || quant > 63 || capacity < 12)
        return 0;
    bit_writer w = { .output = output, .capacity = capacity & ~(size_t)3, .size = 8 };
    unsigned code = 0;
    for (size_t group = 0; group < sizeof(ac_groups) / sizeof(ac_groups[0]); ++group) {
        for (unsigned i = 0; i < ac_groups[group].count; ++i, ++code) {
            unsigned run = (unsigned)ac_codes[code] >> 10;
            unsigned level = (unsigned)ac_codes[code] & 1023;
            w.ac[run][level].value = ac_groups[group].first + i;
            w.ac[run][level].bits = ac_groups[group].bits;
        }
    }
    for (unsigned x = 0; x < picture->width; x += 16) {
        for (unsigned y = 0; y < picture->height; y += 16) {
            encode_block(&w, picture, 2, x / 2, y / 2, version, quant);
            encode_block(&w, picture, 1, x / 2, y / 2, version, quant);
            for (unsigned block = 0; block < 4; ++block)
                encode_block(&w, picture, 0, x + (block & 1) * 8, y + (block >> 1) * 8,
                             version, quant);
            if (w.failed || (w.codes + 1) / 2 > word_limit)
                return 0;
        }
    }
    put_bits(&w, version == 2 ? 0x1ff : 0x3ff, 10);
    if (w.bits)
        put_bits(&w, 0, 16 - w.bits);
    if (w.size % 4)
        put_bits(&w, 0, 16);
    unsigned words = ((w.codes + 63) / 64) * 32;
    if (w.failed || words > word_limit || words > UINT16_MAX)
        return 0;
    write16(output, words);
    write16(output + 2, 0x3800);
    write16(output + 4, quant);
    write16(output + 6, version);
    return w.size;
}
