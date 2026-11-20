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

typedef struct {
    uint64_t hash;
    uint32_t words;
    uint16_t width, height;
} psxrc_video_hint;

/* The caller owns the I/O context and serializes calls on a given reader. */
psxrc *psxrc_open(psxrc_read_at read_at, void *context, uint64_t size);
void psxrc_close(psxrc *image);
unsigned psxrc_track_count(const psxrc *image);
const psxrc_track *psxrc_get_track(const psxrc *image, unsigned number);
uint32_t psxrc_sector_count(const psxrc *image);
uint32_t psxrc_video_sector_count(const psxrc *image);
uint32_t psxrc_audio_sector_count(const psxrc *image);
int psxrc_has_mdec_hints(const psxrc *image);
int psxrc_read_sector(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SECTOR_BYTES]);
/* The optional fingerprint is a 64-bit run-length hash plus a 32-bit word count. */
void psxrc_enable_direct_video(psxrc *image);
const uint8_t *psxrc_mdec_quant_matrix(void);
int psxrc_read_video_hint(psxrc *image, uint32_t lba, psxrc_video_hint *hint);
/* Copy the matched H.264 frame as packed, full-range Y, Cb, and Cr planes. */
int psxrc_copy_video(psxrc *image, uint32_t lba, const psxrc_video_hint *hint,
                     uint8_t *pixels, size_t capacity);
/* An XA template has valid address and subheader fields. Use psxrc_read_sector for guest reads. */
int psxrc_read_xa_template(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SECTOR_BYTES]);
/* Capacity is in interleaved int16_t values. A positive return is the number of sample frames. */
unsigned psxrc_read_xa_pcm(psxrc *image, uint32_t lba, unsigned expected_coding,
                           int16_t *output, size_t capacity);
int psxrc_read_subchannel(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SUB_BYTES]);
int psxrc_has_subchannel(const psxrc *image);
const uint8_t *psxrc_sbi_bitmap(const psxrc *image, size_t *bytes);

#endif
