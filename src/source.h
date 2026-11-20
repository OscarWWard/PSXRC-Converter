#ifndef PSXRC_SOURCE_H
#define PSXRC_SOURCE_H

#include "format.h"
#include <stdio.h>

typedef struct {
    FILE *file;
    uint32_t sector, gap;
    unsigned sub_mode;
} source_part;

typedef struct {
    unsigned track_count, file_count;
    unsigned flags;
    uint32_t sectors;
    psxrc_track track[PSXRC_MAX_TRACKS];
    source_part part[PSXRC_MAX_TRACKS];
    FILE *files[PSXRC_MAX_TRACKS];
    FILE *sub;
    uint8_t *sbi;
    void *chd;
    uint8_t *chd_buffer;
    uint32_t chd_hunk, chd_sectors_per_hunk;
} disc_source;

int source_open(disc_source *source, const char *path);
void source_close(disc_source *source);
int source_read(disc_source *source, uint32_t lba, uint8_t *bytes);
void source_cooked_header(uint8_t *bytes, uint32_t lba);
#ifdef PSXRC_HAVE_CHD
int source_chd_open(disc_source *source, const char *path);
void source_chd_close(disc_source *source);
int source_chd_read(disc_source *source, unsigned track, uint32_t lba, uint8_t *bytes);
#endif

#endif
