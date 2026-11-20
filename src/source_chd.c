/* CD CHDs pad every track to a four-frame boundary on disk.
 * https://github.com/mamedev/mame/blob/master/src/lib/util/cdrom.cpp
 */
#include "source.h"
#include <libchdr/chd.h>
#include <stdlib.h>
#include <string.h>

#define CD_UNIT (PSXRC_SECTOR_BYTES + PSXRC_SUB_BYTES)

static int track_mode(const char *type)
{
    if (!strcmp(type, "AUDIO"))
        return PSXRC_CDDA;
    if (!strcmp(type, "MODE1"))
        return PSXRC_COOKED_MODE1;
    if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW"))
        return PSXRC_RAW_DATA;
    return -1;
}

static int sub_mode(const char *type)
{
    if (!strcmp(type, "NONE"))
        return 0;
    if (!strcmp(type, "RW"))
        return 1;
    if (!strcmp(type, "RW_RAW"))
        return 2;
    return -1;
}

int source_chd_open(disc_source *source, const char *path)
{
    chd_file *chd;
    if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE)
        return 0;
    source->chd = chd;
    const chd_header *header = chd_get_header(chd);
    if (!header || header->unitbytes != CD_UNIT || !header->hunkbytes ||
        header->hunkbytes % CD_UNIT || header->hunkbytes > CD_UNIT * 512)
        return 0;
    source->chd_buffer = malloc(header->hunkbytes);
    if (!source->chd_buffer)
        return 0;
    source->chd_hunk = PSXRC_NO_INDEX;
    source->chd_sectors_per_hunk = header->hunkbytes / CD_UNIT;
    uint32_t physical = 0;
    for (unsigned i = 0; ; ++i) {
        char meta[256], type[32], sub[32], pgtype[32], pgsub[32];
        int number, frames, pregap = 0, postgap = 0;
        uint32_t length = 0;
        chd_error error = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta) - 1,
                                           &length, NULL, NULL);
        int newer = error == CHDERR_NONE;
        if (error == CHDERR_METADATA_NOT_FOUND)
            error = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta) - 1,
                                     &length, NULL, NULL);
        if (error == CHDERR_METADATA_NOT_FOUND)
            break;
        if (error != CHDERR_NONE || i == PSXRC_MAX_TRACKS || length >= sizeof(meta))
            return 0;
        meta[length] = 0;
        if (newer) {
            if (sscanf(meta, "TRACK:%9d TYPE:%31s SUBTYPE:%31s FRAMES:%9d PREGAP:%9d PGTYPE:%31s PGSUB:%31s POSTGAP:%9d",
                       &number, type, sub, &frames, &pregap, pgtype, pgsub, &postgap) != 8)
                return 0;
        } else if (sscanf(meta, "TRACK:%9d TYPE:%31s SUBTYPE:%31s FRAMES:%9d",
                          &number, type, sub, &frames) != 4) {
            return 0;
        }
        int mode = track_mode(type), subcode = sub_mode(sub);
        if (number != (int)i + 1 || mode < 0 || subcode < 0 || frames <= 0 ||
            (unsigned)frames > PSXRC_MAX_SECTORS || pregap < 0 ||
            (unsigned)pregap > PSXRC_MAX_SECTORS || postgap)
            return 0;
        int stored_gap = pregap && pgtype[0] == 'V';
        if (stored_gap && (pregap >= frames || track_mode(pgtype + 1) != mode ||
                           sub_mode(pgsub) != subcode))
            return 0;
        uint32_t synthetic_gap = stored_gap ? 0 : (uint32_t)pregap;
        uint32_t logical_length = (uint32_t)frames + synthetic_gap;
        if (logical_length > PSXRC_MAX_SECTORS - source->sectors ||
            (uint64_t)physical + (unsigned)frames > header->unitcount)
            return 0;
        source->track[i] = (psxrc_track){ source->sectors, source->sectors + (unsigned)pregap,
                                         source->sectors + logical_length, (enum psxrc_mode)mode };
        source->part[i] = (source_part){ NULL, physical, synthetic_gap, (unsigned)subcode };
        source->sectors += logical_length;
        source->track_count++;
        physical += ((unsigned)frames + 3) & ~3u;
        if (subcode)
            source->flags |= PSXRC_FLAG_SUBCHANNEL;
    }
    return source->track_count != 0;
}

void source_chd_close(disc_source *source)
{
    if (source->chd)
        chd_close(source->chd);
    free(source->chd_buffer);
}

int source_chd_read(disc_source *source, unsigned i, uint32_t lba, uint8_t *bytes)
{
    const psxrc_track *track = &source->track[i];
    const source_part *part = &source->part[i];
    uint32_t relative = lba - track->index0;
    if (relative < part->gap) {
        memset(bytes, 0, PSXRC_SECTOR_BYTES);
        return 1;
    }
    uint32_t sector = part->sector + relative - part->gap;
    uint32_t hunk = sector / source->chd_sectors_per_hunk;
    if (hunk != source->chd_hunk) {
        source->chd_hunk = PSXRC_NO_INDEX;
        if (chd_read(source->chd, hunk, source->chd_buffer) != CHDERR_NONE)
            return 0;
        source->chd_hunk = hunk;
    }
    const uint8_t *unit = source->chd_buffer + sector % source->chd_sectors_per_hunk * CD_UNIT;
    if (track->mode == PSXRC_COOKED_MODE1) {
        source_cooked_header(bytes, lba);
        memcpy(bytes + 24, unit, 2048);
    } else if (track->mode == PSXRC_CDDA) {
        for (unsigned j = 0; j < PSXRC_SECTOR_BYTES; j += 2) {
            bytes[j] = unit[j + 1];
            bytes[j + 1] = unit[j];
        }
    } else {
        memcpy(bytes, unit, PSXRC_SECTOR_BYTES);
    }
    if (part->sub_mode) {
        uint8_t *sub = bytes + PSXRC_SECTOR_BYTES + 1;
        bytes[PSXRC_SECTOR_BYTES] = 1;
        memcpy(sub, unit + PSXRC_SECTOR_BYTES, PSXRC_SUB_BYTES);
        if (part->sub_mode == 2) {
            uint8_t q[12] = { 0 };
            for (unsigned j = 0; j < PSXRC_SUB_BYTES; ++j)
                if (sub[j] & 0x40)
                    q[j / 8] |= (uint8_t)(1 << (7 - j % 8));
            memcpy(sub + 12, q, sizeof(q));
        }
    }
    return 1;
}
