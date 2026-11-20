#ifndef PSXRC_H
#define PSXRC_H

#include <stddef.h>
#include <stdint.h>

#define PSXRC_SECTOR_BYTES 2352
#define PSXRC_SUB_BYTES 96
#define PSXRC_MAX_TRACKS 99

typedef struct psxrc psxrc;
typedef int (*psxrc_read_at)(void *context, uint64_t offset, void *output, size_t size);

enum psxrc_mode { PSXRC_RAW_DATA, PSXRC_COOKED_MODE1, PSXRC_CDDA };
typedef struct {
    uint32_t index0, index1, end;
    enum psxrc_mode mode;
} psxrc_track;

/* The caller owns the I/O context and serializes calls on a given reader. */
psxrc *psxrc_open(psxrc_read_at read_at, void *context, uint64_t size);
void psxrc_close(psxrc *image);
unsigned psxrc_track_count(const psxrc *image);
const psxrc_track *psxrc_get_track(const psxrc *image, unsigned number);
uint32_t psxrc_sector_count(const psxrc *image);
uint32_t psxrc_video_sector_count(const psxrc *image);
uint32_t psxrc_audio_sector_count(const psxrc *image);
int psxrc_read_sector(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SECTOR_BYTES]);
int psxrc_read_subchannel(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SUB_BYTES]);
int psxrc_has_subchannel(const psxrc *image);
const uint8_t *psxrc_sbi_bitmap(const psxrc *image, size_t *bytes);

#endif
