#include "bytes.h"
#include "str.h"
#include "xa.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A standalone 37.8 kHz mono XA stream at its single-speed cadence. */
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *file = fopen(argv[1], "wb");
    if (!file)
        return 1;
    int result = 0;
    for (unsigned lba = 0; lba < 96; ++lba) {
        uint8_t sector[STR_SECTOR_BYTES] = { 0 };
        if (lba % 8 == 0) {
            int16_t pcm[XA_MAX_SAMPLES];
            for (unsigned i = 0; i < XA_MAX_SAMPLES; ++i) {
                double t = (lba / 8 * XA_MAX_SAMPLES + i) / 37800.0;
                pcm[i] = (int16_t)(12000 * sin(1130 * t + 446 * t * t) + 7000 * sin(1931 * t));
            }
            memset(sector + 1, 255, 10);
            sector[13] = (uint8_t)(2 + lba / 75);
            unsigned frame = lba % 75;
            sector[14] = (uint8_t)((frame / 10) * 16 + frame % 10);
            sector[15] = 2;
            sector[18] = sector[22] = lba == 88 ? 0xe5 : 0x64;
            if (!xa_encode(pcm, 1, sector + 24)) {
                result = 1;
                break;
            }
            write32(sector + 2348, str_edc(sector + 16, 2332));
        }
        if (fwrite(sector, 1, sizeof(sector), file) != sizeof(sector)) {
            result = 1;
            break;
        }
    }
    return fclose(file) != 0 || result;
}
