#include "bytes.h"
#include "file_io.h"
#include "writer.h"
#include <libavutil/log.h>
#include <stdlib.h>
#include <string.h>

static int read_image(const char *command, const char *path, const char *output)
{
    FILE *file = fopen(path, "rb");
    uint64_t size = 0;
    psxrc *image = file && file_size(file, &size) ? psxrc_open(file_read_at, file, size) : NULL;
    int result = 0;
    char *temp = NULL;
    FILE *out = NULL;
    if (!image) {
        fputs("Could not open the PSXRC image.\n", stderr);
        goto done;
    }
    unsigned tracks = psxrc_track_count(image);
    uint32_t sectors = psxrc_sector_count(image);
    if (!strcmp(command, "inspect")) {
        size_t sbi_bytes;
        const uint8_t *sbi = psxrc_sbi_bitmap(image, &sbi_bytes);
        printf("Sectors: %u\nSTR replacement sectors: %u\nAudio replacement sectors: %u\n"
               "Subchannel: %s\nSBI: %s\n",
               sectors, psxrc_video_sector_count(image), psxrc_audio_sector_count(image),
               psxrc_has_subchannel(image) ? "yes" : "no",
               sbi ? "yes" : "no");
        for (unsigned i = 1; i <= tracks; ++i) {
            const psxrc_track *t = psxrc_get_track(image, i);
            const char *mode = t->mode == PSXRC_CDDA ? "CDDA" :
                               t->mode == PSXRC_COOKED_MODE1 ? "MODE1/2048" : "raw data";
            printf("Track %u: %s, INDEX 00 %u, INDEX 01 %u, end %u\n",
                   i, mode, t->index0, t->index1, t->end);
        }
        result = 1;
        goto done;
    }
    if (output) {
        if (tracks != 1 || psxrc_get_track(image, 1)->index1 ||
            psxrc_get_track(image, 1)->mode != PSXRC_RAW_DATA) {
            fputs("BIN export requires one raw data track beginning at LBA 0.\n", stderr);
            goto done;
        }
        out = file_create_temp(output, &temp);
        if (!out)
            goto done;
    }
    for (uint32_t i = 0; i < sectors; ++i) {
        uint8_t sector[PSXRC_SECTOR_BYTES];
        if (!psxrc_read_sector(image, i, sector)) {
            fprintf(stderr, "Could not read sector %u.\n", i);
            goto done;
        }
        if (out && fwrite(sector, 1, sizeof(sector), out) != sizeof(sector))
            goto done;
    }
    if (out) {
        int closed = fclose(out);
        out = NULL;
        if (closed != 0 || !file_publish(temp, output))
            goto done;
    }
    printf("Verified %u sectors.\n", sectors);
    result = 1;
done:
    if (out)
        fclose(out);
    if (temp) {
        if (!result)
            remove(temp);
        free(temp);
    }
    psxrc_close(image);
    if (file)
        fclose(file);
    return result;
}

static int parse_options(int argc, char **argv, convert_options *options)
{
    const char *preset = "balanced";
    int no_video = 0;
    options->auto_video = 1;
    options->cdda = options->xa = 1;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            preset = argv[++i];
        } else if ((!strcmp(argv[i], "--str") || !strcmp(argv[i], "--keep-lba")) && i + 1 < argc) {
            int keep = !strcmp(argv[i], "--keep-lba");
            unsigned first, count;
            char extra;
            if (sscanf(argv[++i], "%6u:%6u%c", &first, &count, &extra) != 2 || !count ||
                first >= PSXRC_MAX_SECTORS || count > PSXRC_MAX_SECTORS - first)
                return 0;
            if (keep) {
                if (options->kept >= sizeof(options->keep) / sizeof(options->keep[0]))
                    return 0;
                options->keep[options->kept++] = (lba_range){ first, count };
            } else {
                if (options->count)
                    return 0;
                options->first = first;
                options->count = count;
            }
        } else if (!strcmp(argv[i], "--no-video")) {
            no_video = 1;
        } else if (!strcmp(argv[i], "--no-cdda")) {
            options->cdda = 0;
        } else if (!strcmp(argv[i], "--no-xa")) {
            options->xa = 0;
        } else {
            return 0;
        }
    }
    options->zstd_level = 8;
    options->min_saving = 10;
    if (!strcmp(preset, "high")) {
        options->crf = 17;
        options->mse_limit = 65;
        options->audio_bitrate = 160000;
        options->audio_ratio = 200;
    } else if (!strcmp(preset, "balanced")) {
        options->crf = 20;
        options->mse_limit = 103;
        options->audio_bitrate = 112000;
        options->audio_ratio = 128;
    } else if (!strcmp(preset, "compact")) {
        options->crf = 26;
        options->mse_limit = 205;
        options->zstd_level = 15;
        options->audio_bitrate = 64000;
        options->audio_ratio = 64;
    } else if (!strcmp(preset, "lossless")) {
        no_video = 1;
        options->cdda = options->xa = 0;
    } else {
        return 0;
    }
    if (no_video)
        options->auto_video = options->count = 0;
    return 1;
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_ERROR);
    if (argc >= 4 && !strcmp(argv[1], "convert")) {
        convert_options options = { 0 };
        if (parse_options(argc, argv, &options)) {
            disc_source source;
            int result = source_open(&source, argv[2]);
            if (!result) {
                fputs("Unsupported or unreadable disc. Expected a binary CUE, raw BIN, cooked ISO"
#ifdef PSXRC_HAVE_CHD
                      ", or supported standalone CD CHD"
#endif
                      ".\n", stderr);
            } else if (options.count && (options.first >= source.sectors ||
                       options.count > source.sectors - options.first)) {
                fputs("The STR range extends beyond the disc.\n", stderr);
                result = 0;
            } else {
                for (unsigned i = 0; i < options.kept; ++i) {
                    const lba_range *range = &options.keep[i];
                    if (range->first >= source.sectors || range->count > source.sectors - range->first) {
                        fputs("A keep-LBA range extends beyond the disc.\n", stderr);
                        result = 0;
                        break;
                    }
                }
                if (result) {
                    result = write_image(&source, argv[3], &options);
                    if (!result)
                        fputs("Conversion or output verification failed. Output must be a new writable file.\n", stderr);
                }
            }
            source_close(&source);
            return result ? 0 : 1;
        }
    }
    if (argc == 3 && (!strcmp(argv[1], "inspect") || !strcmp(argv[1], "verify")))
        return read_image(argv[1], argv[2], NULL) ? 0 : 1;
    if (argc == 4 && !strcmp(argv[1], "extract-bin"))
        return read_image(argv[1], argv[2], argv[3]) ? 0 : 1;
    fputs("Usage:\n"
          "  psxrc convert INPUT OUTPUT.psxrc [--preset lossless|high|balanced|compact]\n"
          "      [--str FIRST_LBA:SECTOR_COUNT] [--keep-lba FIRST_LBA:SECTOR_COUNT]\n"
          "      [--no-video] [--no-xa] [--no-cdda]\n"
          "  psxrc inspect INPUT.psxrc\n"
          "  psxrc verify INPUT.psxrc\n"
          "  psxrc extract-bin INPUT.psxrc OUTPUT.bin\n", stderr);
    return 2;
}
