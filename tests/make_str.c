#include "bs.h"
#include "bytes.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 320, HEIGHT = 224, FRAMES = 12 };

static unsigned bcd(unsigned n) { return n / 10 * 16 + n % 10; }

static int write_sector(FILE *file, uint32_t lba, unsigned number, unsigned part,
                        unsigned chunks, const uint8_t *bs, size_t size)
{
    uint8_t sector[STR_SECTOR_BYTES] = { 0 };
    memset(sector + 1, 0xff, 10);
    unsigned address = lba + 150;
    sector[12] = (uint8_t)bcd(address / 4500);
    sector[13] = (uint8_t)bcd(address / 75 % 60);
    sector[14] = (uint8_t)bcd(address % 75);
    sector[15] = 2;
    sector[18] = sector[22] = bs ? (read16(bs + 6) == 2 ? 0x42 : 0x62) : 0x64;
    if (bs) {
        write32(sector + 24, UINT32_C(0x80010160));
        write16(sector + 28, part);
        write16(sector + 30, chunks);
        write32(sector + 32, number);
        write32(sector + 36, (uint32_t)size);
        write16(sector + 40, WIDTH);
        write16(sector + 42, HEIGHT);
        memcpy(sector + 44, bs, 8);
        size_t offset = part * STR_CHUNK_BYTES;
        size_t n = size - offset;
        if (n > STR_CHUNK_BYTES)
            n = STR_CHUNK_BYTES;
        memcpy(sector + 56, bs + offset, n);
    } else {
        sector[113] = (uint8_t)lba;
    }
    str_write_integrity(sector);
    return fwrite(sector, 1, sizeof(sector), file) == sizeof(sector);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    unsigned version = (unsigned)atoi(argv[2]);
    uint8_t y[WIDTH * HEIGHT], u[WIDTH * HEIGHT / 4], v[sizeof(u)];
    uint8_t bs[STR_MAX_FRAME_BYTES];
    bs_picture p = { { y, u, v }, { WIDTH, WIDTH / 2, WIDTH / 2 }, WIDTH, HEIGHT };
    FILE *file = fopen(argv[1], "wb");
    if (!file)
        return 1;
    uint32_t lba = 0;
    int ok = 1;
    for (unsigned f = 0; f < FRAMES && ok; ++f) {
        for (unsigned row = 0; row < HEIGHT; ++row) {
            for (unsigned x = 0; x < WIDTH; ++x) {
                uint32_t seed = (x + row * WIDTH) * UINT32_C(0x811c9dc5);
                seed ^= seed >> 14;
                unsigned wave = (x + row * 2 + f * 4) % 192;
                if (wave > 95)
                    wave = 191 - wave;
                y[row * WIDTH + x] = (uint8_t)(32 + wave + (seed % 15) + (row > 100 && x > 170 ? 60 : 0));
            }
        }
        for (unsigned i = 0; i < sizeof(u); ++i) {
            u[i] = (uint8_t)(96 + (i / (WIDTH / 2) + f) % 64);
            v[i] = (uint8_t)(106 + (i % (WIDTH / 2) + f) % 56);
        }
        size_t n = bs_encode(&p, version, 1, 32736, bs, sizeof(bs));
        unsigned chunks = (unsigned)((n + STR_CHUNK_BYTES - 1) / STR_CHUNK_BYTES);
        ok = n && chunks;
        for (unsigned i = 0; i < chunks && ok; ++i) {
            ok = write_sector(file, lba++, f + 1, i, chunks, bs, n);
            if ((i & 1) == 0)
                ok = ok && write_sector(file, lba++, 0, 0, 0, NULL, 0);
        }
    }
    return fclose(file) != 0 || !ok;
}
