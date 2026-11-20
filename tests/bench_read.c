#include "file_io.h"
#include "source.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    disc_source source;
    FILE *file;
    psxrc *image;
} input;

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return (double)ticks.QuadPart * 1000 / frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
#endif
}

static int start(input *in, const char *path, int is_psxrc)
{
    *in = (input){ 0 };
    if (!is_psxrc)
        return source_open(&in->source, path);
    uint64_t size;
    in->file = fopen(path, "rb");
    if (in->file && file_size(in->file, &size))
        in->image = psxrc_open(file_read_at, in->file, size);
    return in->image != NULL;
}

static void stop(input *in)
{
    psxrc_close(in->image);
    if (in->file)
        fclose(in->file);
    source_close(&in->source);
}

static int read_one(input *in, uint32_t lba, uint8_t *sector)
{
    return in->image ? psxrc_read_sector(in->image, lba, sector)
                     : source_read(&in->source, lba, sector);
}

static int cmp_sample(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double p95(double *samples, size_t count)
{
    qsort(samples, count, sizeof(*samples), cmp_sample);
    return samples[(count * 95 - 1) / 100];
}

static int measure(const char *path, int is_psxrc, uint32_t first, uint32_t count,
                   const uint32_t *seeks, size_t num_seeks)
{
    enum { PASSES = 8, SEEK_READS = 96 };
    double cold[SEEK_READS];
    double *seq = malloc((size_t)count * PASSES * sizeof(*seq));
    if (!seq)
        return 0;
    input in;
    uint8_t data[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
    int ok = start(&in, path, is_psxrc);
    for (unsigned pass = 0; pass < PASSES && ok; ++pass) {
        for (uint32_t n = 0; n < count && ok; ++n) {
            double start_time = now_ms();
            ok = read_one(&in, first + n, data);
            seq[(size_t)pass * count + n] = now_ms() - start_time;
        }
    }
    stop(&in);
    uint32_t rng = 7;
    for (unsigned i = 0; i < SEEK_READS && ok; ++i) {
        rng = rng * 1664525 + 1013904223;
        ok = start(&in, path, is_psxrc);
        if (ok) {
            double start_time = now_ms();
            ok = read_one(&in, seeks[(rng >> 8) % num_seeks], data);
            cold[i] = now_ms() - start_time;
        }
        stop(&in);
    }
    if (ok)
        printf("%s\n  sequential p95 %.3f ms\n  cold reader seek p95 %.3f ms\n",
               path, p95(seq, (size_t)count * PASSES), p95(cold, SEEK_READS));
    free(seq);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned first, count;
    char extra;
    int all = argc == 5 && !strcmp(argv[4], "--all-sectors");
    if ((argc != 4 && !all) || sscanf(argv[3], "%6u:%6u%c", &first, &count, &extra) != 2 ||
        !count || first >= PSXRC_MAX_SECTORS || count > PSXRC_MAX_SECTORS - first) {
        fputs("Usage: psxrc_bench BASELINE.chd IMAGE.psxrc FIRST_LBA:COUNT [--all-sectors]\n", stderr);
        return 2;
    }
    uint32_t *seeks = malloc(count * sizeof(*seeks));
    if (!seeks)
        return 1;
    size_t num_seeks = 0;
    input in;
    int ok = start(&in, argv[1], 0);
    for (uint32_t i = 0; i < count && ok; ++i) {
        uint8_t data[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
        ok = read_one(&in, first + i, data);
        if (ok && (all || str_has_signature(data)))
            seeks[num_seeks++] = first + i;
    }
    stop(&in);
    ok = ok && num_seeks && measure(argv[1], 0, first, count, seeks, num_seeks) &&
         measure(argv[2], 1, first, count, seeks, num_seeks);
    if (!ok)
        fputs("Could not complete the benchmark on the selected sector range.\n", stderr);
    free(seeks);
    return ok ? 0 : 1;
}
