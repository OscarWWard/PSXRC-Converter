#include "audio_convert.h"
#include "audio.h"
#include "bytes.h"
#include "file_io.h"
#include "xa.h"
#include <libavutil/adler32.h>
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

static int encode_stream(disc_source *source, const convert_options *options, audio_plan *plan,
                         enum audio_kind kind, unsigned coding, const uint32_t *lbas, uint32_t count)
{
    unsigned rate = audio_rate(kind, coding), channels = audio_channels(kind, coding);
    unsigned preroll = rate * 8 / 100 * channels;
    unsigned values_per_unit = audio_samples_per_unit(kind, coding) * channels;
    unsigned group_size = audio_units_per_group(kind, coding);
    int16_t *pcm = calloc(preroll + values_per_unit * group_size, sizeof(*pcm));
    uint8_t *raw = malloc(group_size * AUDIO_UNIT_BYTES);
    uint8_t *compressed = malloc(group_size * AUDIO_UNIT_BYTES);
    uint8_t packet[AUDIO_MAX_GROUP];
    uint32_t initial_groups = plan->groups;
    uint64_t initial_size = plan->size;
    uint64_t baseline = 0;
    xa_predictors predictors = { 0 };
    int result = 0;
    if (!pcm || !raw || !compressed || !file_seek(plan->data, plan->size)) {
        result = -1;
        goto done;
    }
    for (uint32_t first = 0; first < count;) {
        unsigned units = count - first < group_size ? count - first : group_size;
        for (unsigned i = 0; i < units; ++i) {
            uint8_t sector[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
            uint32_t lba = lbas[first + i];
            if (!source_read(source, lba, sector)) {
                result = -1;
                goto done;
            }
            if (keep_sector(source, options, lba))
                goto done;
            int16_t *out = pcm + preroll + i * values_per_unit;
            uint8_t *original = raw + i * AUDIO_UNIT_BYTES;
            if (kind == AUDIO_XA4) {
                if (!xa_header(sector) || sector[19] != coding ||
                    !xa_decode(sector + 24, channels, &predictors, out))
                    goto done;
                memcpy(original, sector + 24, XA_DATA_BYTES);
                memset(original + XA_DATA_BYTES, 0, AUDIO_UNIT_BYTES - XA_DATA_BYTES);
            } else {
                memcpy(original, sector, AUDIO_UNIT_BYTES);
                for (unsigned j = 0; j < values_per_unit; ++j)
                    out[j] = (int16_t)read16(sector + j * 2);
            }
        }
        unsigned bitrate = options->audio_bitrate;
        if (kind == AUDIO_XA4 && channels == 1)
            bitrate /= 2;
        size_t bytes = audio_pack(kind, coding, pcm, units, bitrate, options->audio_ratio, packet);
        if (!bytes)
            goto done;
        if (fwrite(packet, 1, bytes, plan->data) != bytes) {
            result = -1;
            goto done;
        }
        size_t raw_bytes = units * AUDIO_UNIT_BYTES;
        size_t lossless = ZSTD_compress(compressed, raw_bytes, raw, raw_bytes, options->zstd_level);
        baseline += ZSTD_isError(lossless) ? raw_bytes : lossless;
        for (unsigned i = 0; i < units; ++i) {
            uint32_t lba = lbas[first + i];
            plan->slot[lba] = plan->groups * AUDIO_MAX_UNITS + i;
            plan->xa[lba] = kind == AUDIO_XA4;
        }
        plan->group[plan->groups++] =
            (psxrc_record){ plan->size, (uint32_t)bytes, av_adler32_update(1, packet, bytes) };
        plan->size += bytes;
        memmove(pcm, pcm + units * values_per_unit, preroll * sizeof(*pcm));
        first += units;
    }
    uint64_t transformed = plan->size - initial_size + (plan->groups - initial_groups) * PSXRC_RECORD_BYTES +
                           (uint64_t)count * PSXRC_MAP_BYTES;
    if (transformed * 100 < baseline * (100 - options->min_saving)) {
        plan->sectors += count;
        result = 1;
    }
done:
    if (result <= 0) {
        plan->groups = initial_groups;
        plan->size = initial_size;
        for (uint32_t i = 0; i < count; ++i) {
            plan->slot[lbas[i]] = PSXRC_NO_INDEX;
            plan->xa[lbas[i]] = 0;
        }
    }
    free(pcm);
    free(raw);
    free(compressed);
    return result;
}

typedef struct {
    uint32_t lba, step;
    uint8_t coding, end, blocked;
} xa_tail;

typedef struct {
    uint32_t lba;
    unsigned key, coding;
} xa_start;

static int scan_xa(disc_source *source, audio_plan *plan, const convert_options *options,
                   uint32_t first, uint32_t end, uint32_t *lbas)
{
    const size_t count = end - first;
    uint32_t *next = malloc(count * sizeof(*next));
    xa_start *starts = malloc(count * sizeof(*starts));
    xa_tail *tails = calloc(256 * 32, sizeof(*tails));
    unsigned found = 0;
    int result = 0;
    if (!next || !starts || !tails)
        goto done;
    memset(next, 0xff, count * sizeof(*next));
    for (uint32_t lba = first; lba < end; ++lba) {
        uint8_t sector[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
        if (!source_read(source, lba, sector))
            goto done;
        if (sector[15] != 2 || (sector[18] & 0x6e) != 0x64 || sector[17] >= 32)
            continue;
        unsigned key = sector[16] * 32 + sector[17];
        xa_tail *tail = &tails[key];
        if (!xa_header(sector) || keep_sector(source, options, lba)) {
            tail->blocked = 1;
            continue;
        }
        unsigned coding = sector[19];
        unsigned cadence = audio_samples_per_unit(AUDIO_XA4, coding) * 75 / audio_rate(AUDIO_XA4, coding);
        uint32_t gap = lba - tail->lba;
        int connect = tail->end == 1 && tail->coding == coding &&
                      (gap == cadence || gap == cadence * 2) && (!tail->step || tail->step == gap);
        if (connect) {
            next[tail->lba - first] = lba;
        } else {
            if (tail->end == 1)
                tail->blocked = 1;
            starts[found++] = (xa_start){ lba, key, coding };
        }
        *tail = (xa_tail){ lba, connect ? gap : 0, (uint8_t)coding,
                          sector[18] & 0x81 ? 2 : 1, tail->blocked };
    }
    for (unsigned i = 0; i < found; ++i) {
        const xa_start *start = &starts[i];
        if (tails[start->key].blocked)
            continue;
        unsigned units = 0;
        for (uint32_t n = start->lba; n != PSXRC_NO_INDEX; n = next[n - first])
            lbas[units++] = n;
        if (units < 3)
            continue;
        int status = encode_stream(source, options, plan, AUDIO_XA4, start->coding, lbas, units);
        if (status < 0)
            goto done;
        if (!status)
            fprintf(stderr, "XA at LBA %u preserved losslessly after audio checks.\n", start->lba);
    }
    result = 1;
done:
    free(tails);
    free(starts);
    free(next);
    return result;
}

int audio_plan_build(disc_source *source, audio_plan *plan, const convert_options *options)
{
    uint32_t *lbas = malloc(source->sectors * sizeof(*lbas));
    if (!lbas)
        return 0;
    int result = 1;
    for (unsigned i = 0; i < source->track_count && result; ++i) {
        const psxrc_track *t = &source->track[i];
        uint32_t count = t->end - t->index1;
        if (options->cdda && t->mode == PSXRC_CDDA) {
            for (uint32_t n = 0; n < count; ++n)
                lbas[n] = t->index1 + n;
            int status = encode_stream(source, options, plan, AUDIO_CDDA, 0, lbas, count);
            result = status >= 0;
            if (!status)
                fprintf(stderr, "CDDA track %u preserved losslessly after audio checks.\n", i + 1);
        } else if (options->xa && t->mode == PSXRC_RAW_DATA) {
            result = scan_xa(source, plan, options, t->index1, t->end, lbas);
        }
    }
    free(lbas);
    return result;
}
