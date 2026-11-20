#include "writer.h"
#include "file_io.h"
#include "bytes.h"
#include "filters.h"
#include "str.h"
#include "video_convert.h"
#include "audio.h"
#include "audio_convert.h"
#include "xa.h"
#include <libavutil/adler32.h>
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *data;
    psxrc_record *group;
    uint32_t *slot;
    uint32_t groups, sectors;
    uint64_t size;
} video_plan;

int keep_sector(const disc_source *source, const convert_options *options, uint32_t lba)
{
    if (source->sbi && (source->sbi[lba / 8] & (1 << (lba % 8))))
        return 1;
    for (unsigned i = 0; i < options->kept; ++i)
        if (lba >= options->keep[i].first && lba - options->keep[i].first < options->keep[i].count)
            return 1;
    return 0;
}

static void put_record(uint8_t *p, uint64_t offset, uint32_t bytes, uint32_t checksum)
{
    write64(p, offset);
    write32(p + 8, bytes);
    write32(p + 12, checksum);
}

static int append_group(video_plan *plan, video_frame *frame, unsigned count,
                        const convert_options *options)
{
    if (!video_convert(frame, count, options))
        return 0;
    uint8_t group_header[8] = { 0 };
    write16(group_header, count);
    write16(group_header + 2, frame[0].header.width);
    write16(group_header + 4, frame[0].header.height);
    if (fwrite(group_header, 1, sizeof(group_header), plan->data) != sizeof(group_header))
        return 0;
    uint32_t size = sizeof(group_header);
    uint32_t checksum = av_adler32_update(1, group_header, sizeof(group_header));
    for (unsigned i = 0; i < count; ++i) {
        video_frame *f = &frame[i];
        size_t packet_size = (size_t)f->packet->size;
        uint8_t header[PSXRC_FRAME_BYTES];
        header[0] = (uint8_t)f->quant;
        header[1] = (uint8_t)f->header.version;
        write16(header + 2, f->header.words);
        write32(header + 4, (uint32_t)packet_size);
        write32(header + 8, f->header.length);
        write32(header + 12, (uint32_t)f->rebuilt_size);
        write32(header + 16, av_adler32_update(1, f->rebuilt, f->rebuilt_size));
        if (fwrite(header, 1, sizeof(header), plan->data) != sizeof(header) ||
            fwrite(f->packet->data, 1, packet_size, plan->data) != packet_size)
            return 0;
        checksum = av_adler32_update(checksum, header, sizeof(header));
        checksum = av_adler32_update(checksum, f->packet->data, packet_size);
        size += (uint32_t)(sizeof(header) + packet_size);
        for (unsigned n = 0; n < f->header.chunks; ++n) {
            plan->slot[f->lba[n]] = plan->groups * PSXRC_GROUP_FRAMES + i;
            ++plan->sectors;
        }
    }
    if (options->mdec_hints) {
        for (unsigned i = 0; i < count; ++i) {
            uint8_t hint[PSXRC_MDEC_HINT_BYTES];
            write64(hint, frame[i].mdec.hash);
            write32(hint + 8, frame[i].mdec.words);
            if (fwrite(hint, 1, sizeof(hint), plan->data) != sizeof(hint))
                return 0;
            checksum = av_adler32_update(checksum, hint, sizeof(hint));
            size += sizeof(hint);
        }
    }
    plan->group[plan->groups++] = (psxrc_record){ plan->size, size, checksum };
    plan->size += size;
    return 1;
}

static int scan_video(disc_source *source, video_plan *plan, const convert_options *options,
                      uint32_t first, uint32_t count)
{
    int in_track = 0;
    for (unsigned i = 0; i < source->track_count; ++i) {
        const psxrc_track *t = &source->track[i];
        in_track |= t->mode == PSXRC_RAW_DATA && first >= t->index1 && first + count <= t->end;
    }
    if (!in_track)
        return 0;
    video_frame frame[PSXRC_GROUP_FRAMES] = { 0 };
    unsigned complete = 0, chunk = 0;
    str_header previous = { 0 };
    int result = 0;
    uint32_t initial = plan->sectors;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t lba = first + i;
        uint8_t sector[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
        if (!source_read(source, lba, sector))
            goto done;
        if (!str_has_signature(sector))
            continue;
        str_header header;
        if (!str_parse(sector, &header) || keep_sector(source, options, lba))
            goto done;
        video_frame *f = &frame[complete];
        if (!chunk) {
            if (header.chunk || (previous.chunks &&
                (header.file != previous.file || header.channel != previous.channel ||
                 header.width != previous.width || header.height != previous.height ||
                 header.version != previous.version || header.number != previous.number + 1)))
                goto done;
            f->header = header;
            f->original = malloc(header.length);
            if (!f->original)
                goto done;
        } else if (header.chunk != chunk || !str_same_frame(&f->header, &header)) {
            goto done;
        }
        size_t offset = chunk * STR_CHUNK_BYTES;
        size_t n = header.length > offset ? header.length - offset : 0;
        if (n > STR_CHUNK_BYTES)
            n = STR_CHUNK_BYTES;
        if (n)
            memcpy(f->original + offset, sector + 56, n);
        f->lba[chunk++] = lba;
        if (chunk == header.chunks) {
            if (read16(f->original) != header.words || read16(f->original + 2) != 0x3800 ||
                read16(f->original + 4) != header.quant || read16(f->original + 6) != header.version)
                goto done;
            previous = header;
            chunk = 0;
            if (++complete == PSXRC_GROUP_FRAMES) {
                if (!append_group(plan, frame, complete, options))
                    goto done;
                for (unsigned j = 0; j < complete; ++j)
                    video_frame_clear(&frame[j]);
                complete = 0;
            }
        }
    }
    if (!chunk && (!complete || append_group(plan, frame, complete, options)))
        result = plan->sectors > initial;
done:
    for (unsigned i = 0; i < PSXRC_GROUP_FRAMES; ++i)
        video_frame_clear(&frame[i]);
    return result;
}

static unsigned cdda_mask(const disc_source *source, uint32_t first, unsigned count)
{
    unsigned mask = 0;
    for (unsigned t = 0; t < source->track_count; ++t) {
        const psxrc_track *track = &source->track[t];
        if (track->mode == PSXRC_CDDA && track->end > first && track->index0 < first + count)
            for (unsigned i = 0; i < count; ++i)
                if (first + i >= track->index0 && first + i < track->end)
                    mask |= 1u << i;
    }
    return mask;
}

static size_t compress_hunk(uint8_t *out, const uint8_t *raw, unsigned count, unsigned stride,
                            unsigned cdda, int level)
{
    uint8_t filtered[PSXRC_FILTER_BYTES + PSXRC_HUNK_SECTORS * (PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES)];
    size_t n = count * stride + PSXRC_FILTER_BYTES;
    filter_hunk(filtered, raw, count, stride, cdda);
    size_t z = ZSTD_compress(out, n, filtered, n, level);
    if (ZSTD_isError(z) || z >= n) {
        memcpy(out, filtered, n);
        return n;
    }
    return z;
}

/* Check the actual hunk costs of this stream, including its record and LBA map. */
static int check_video_savings(disc_source *source, const video_plan *plan, const convert_options *options,
                               uint32_t first, uint32_t count, uint32_t previous_groups,
                               uint32_t previous_sectors, uint64_t previous_size)
{
    uint64_t before = 0;
    uint64_t after = plan->size - previous_size + (plan->groups - previous_groups) * PSXRC_RECORD_BYTES +
                     (plan->sectors - previous_sectors) * PSXRC_MAP_BYTES;
    unsigned stride = PSXRC_SECTOR_BYTES +
        ((source->flags & PSXRC_FLAG_SUBCHANNEL) ? PSXRC_SUB_RECORD_BYTES : 0);
    uint8_t raw[PSXRC_HUNK_SECTORS * (PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES)];
    uint8_t encoded[sizeof(raw) + PSXRC_FILTER_BYTES];
    uint32_t hunk_start = first - first % PSXRC_HUNK_SECTORS;
    for (; hunk_start < first + count; hunk_start += PSXRC_HUNK_SECTORS) {
        unsigned sectors = source->sectors - hunk_start;
        if (sectors > PSXRC_HUNK_SECTORS)
            sectors = PSXRC_HUNK_SECTORS;
        unsigned affected = 0;
        for (unsigned i = 0; i < sectors; ++i) {
            uint32_t slot = plan->slot[hunk_start + i];
            affected |= slot != PSXRC_NO_INDEX && slot / PSXRC_GROUP_FRAMES >= previous_groups;
        }
        if (!affected)
            continue;
        for (unsigned i = 0; i < sectors; ++i)
            if (!source_read(source, hunk_start + i, raw + i * stride))
                return 0;
        unsigned cdda = cdda_mask(source, hunk_start, sectors);
        before += compress_hunk(encoded, raw, sectors, stride, cdda, options->zstd_level);
        for (unsigned i = 0; i < sectors; ++i) {
            uint32_t slot = plan->slot[hunk_start + i];
            if (slot != PSXRC_NO_INDEX && slot / PSXRC_GROUP_FRAMES >= previous_groups) {
                str_header h;
                if (!str_parse(raw + i * stride, &h))
                    return 0;
                str_make_template(raw + i * stride, &h);
            }
        }
        after += compress_hunk(encoded, raw, sectors, stride, cdda, options->zstd_level);
    }
    return after * 100 < before * (100 - options->min_saving);
}

static int try_video(disc_source *source, video_plan *plan, const convert_options *options,
                     uint32_t first, uint32_t count)
{
    uint32_t groups = plan->groups, sectors = plan->sectors;
    uint64_t size = plan->size;
    if (!file_seek(plan->data, size))
        return 0;
    if (!scan_video(source, plan, options, first, count) ||
        !check_video_savings(source, plan, options, first, count, groups, sectors, size)) {
        for (uint32_t i = first; i < first + count; ++i)
            plan->slot[i] = PSXRC_NO_INDEX;
        plan->groups = groups;
        plan->sectors = sectors;
        plan->size = size;
        fprintf(stderr, "STR at LBA %u preserved losslessly after structure, quality or size checks.\n", first);
    }
    return 1;
}

typedef struct {
    uint32_t first, last;
    unsigned frames;
    str_header header;
    int active, valid;
} str_candidate;

static int finish_candidate(disc_source *source, video_plan *plan, const convert_options *options,
                            const str_candidate *candidate)
{
    if (!candidate->active || !candidate->valid || candidate->frames < 4 ||
        candidate->header.chunk + 1 != candidate->header.chunks)
        return 1;
    return try_video(source, plan, options, candidate->first, candidate->last - candidate->first + 1);
}

static int discover_video(disc_source *source, video_plan *plan, const convert_options *options)
{
    for (unsigned i = 0; i < source->track_count; ++i) {
        const psxrc_track *t = &source->track[i];
        if (t->mode != PSXRC_RAW_DATA)
            continue;
        str_candidate c = { 0 };
        for (uint32_t lba = t->index1; lba < t->end; ++lba) {
            uint8_t sector[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
            if (!source_read(source, lba, sector))
                return 0;
            if (c.active && lba - c.last > 32) {
                if (!finish_candidate(source, plan, options, &c))
                    return 0;
                c.active = 0;
            }
            if (!str_has_signature(sector))
                continue;
            str_header h;
            if (!str_parse(sector, &h)) {
                c.valid = 0;
                continue;
            }
            if (!h.chunk && h.number <= 1 && (!c.active || !c.valid || c.frames > 1)) {
                if (!finish_candidate(source, plan, options, &c))
                    return 0;
                c = (str_candidate){ lba, lba, 1, h, 1, 1 };
                continue;
            }
            if (!c.active || !c.valid)
                continue;
            if (c.header.chunk + 1 < c.header.chunks) {
                c.valid = h.chunk == c.header.chunk + 1 && str_same_frame(&c.header, &h);
            } else {
                c.valid = !h.chunk && h.number == c.header.number + 1 &&
                          h.version == c.header.version && h.file == c.header.file &&
                          h.channel == c.header.channel && h.width == c.header.width &&
                          h.height == c.header.height;
                ++c.frames;
            }
            c.last = lba;
            c.header = h;
        }
        if (!finish_candidate(source, plan, options, &c))
            return 0;
    }
    return 1;
}

static int write_media(FILE *file, FILE *spool, const psxrc_record *groups, uint32_t group_count,
                       const uint32_t *map, uint32_t disc_sectors, uint64_t bytes,
                       uint8_t **index, uint64_t *offset)
{
    if (!group_count)
        return 1;
    uint8_t *p = *index;
    for (uint32_t i = 0; i < group_count; ++i, p += PSXRC_RECORD_BYTES)
        put_record(p, *offset + groups[i].offset, groups[i].bytes, groups[i].checksum);
    for (uint32_t i = 0; i < disc_sectors; ++i) {
        if (map[i] != PSXRC_NO_INDEX) {
            write32(p, i);
            write32(p + 4, map[i]);
            p += PSXRC_MAP_BYTES;
        }
    }
    *index = p;
    *offset += bytes;
    if (!file_seek(spool, 0))
        return 0;
    uint8_t buffer[16384];
    while (bytes) {
        size_t n = bytes < sizeof(buffer) ? (size_t)bytes : sizeof(buffer);
        if (fread(buffer, 1, n, spool) != n || fwrite(buffer, 1, n, file) != n)
            return 0;
        bytes -= n;
    }
    return 1;
}

static void blank_audio(const audio_plan *audio, uint32_t first, unsigned count,
                        unsigned stride, uint8_t *output)
{
    for (unsigned i = 0; i < count; ++i) {
        uint32_t lba = first + i;
        if (audio->slot[lba] == PSXRC_NO_INDEX)
            continue;
        uint8_t *sector = output + i * stride;
        if (audio->xa[lba]) {
            memset(sector + 24, 0, XA_DATA_BYTES);
            if (read32(sector + 2348))
                write32(sector + 2348, str_edc(sector + 16, 2332));
        } else {
            memset(sector, 0, PSXRC_SECTOR_BYTES);
        }
    }
}

static int write_contents(FILE *file, disc_source *source, video_plan *video,
                          audio_plan *audio, const convert_options *options)
{
    uint32_t hunks = (source->sectors + PSXRC_HUNK_SECTORS - 1) / PSXRC_HUNK_SECTORS;
    unsigned stride = PSXRC_SECTOR_BYTES +
        ((source->flags & PSXRC_FLAG_SUBCHANNEL) ? PSXRC_SUB_RECORD_BYTES : 0);
    size_t sbi_bytes = source->sbi ? (source->sectors + 7) / 8 : 0;
    size_t index_bytes = source->track_count * PSXRC_TRACK_BYTES +
        (hunks + video->groups + audio->groups) * PSXRC_RECORD_BYTES +
        (video->sectors + audio->sectors) * PSXRC_MAP_BYTES + sbi_bytes;
    uint8_t *index = calloc(1, index_bytes);
    uint64_t offset = PSXRC_HEADER_BYTES + index_bytes;
    int result = 0;
    if (!index || !file_seek(file, offset))
        goto done;
    uint8_t *p = index;
    for (unsigned i = 0; i < source->track_count; ++i, p += PSXRC_TRACK_BYTES) {
        psxrc_track *t = &source->track[i];
        write32(p, t->index0);
        write32(p + 4, t->index1);
        write32(p + 8, t->end);
        write32(p + 12, t->mode);
    }
    uint64_t video_before = 0, video_after = video->size + video->groups * PSXRC_RECORD_BYTES +
                                             video->sectors * PSXRC_MAP_BYTES;
    uint64_t audio_before = 0, audio_after = audio->size + audio->groups * PSXRC_RECORD_BYTES +
                                             audio->sectors * PSXRC_MAP_BYTES;
    uint8_t raw[PSXRC_HUNK_SECTORS * (PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES)];
    uint8_t encoded[sizeof(raw) + PSXRC_FILTER_BYTES], audio_only[sizeof(raw)];
    for (uint32_t i = 0; i < hunks; ++i, p += PSXRC_RECORD_BYTES) {
        unsigned count = 0, has_video = 0, has_audio = 0;
        uint32_t first = i * PSXRC_HUNK_SECTORS;
        while (count < PSXRC_HUNK_SECTORS && first + count < source->sectors) {
            uint32_t lba = first + count;
            if (!source_read(source, lba, raw + count * stride))
                goto done;
            has_video |= video->groups && video->slot[lba] != PSXRC_NO_INDEX;
            has_audio |= audio->groups && audio->slot[lba] != PSXRC_NO_INDEX;
            ++count;
        }
        unsigned n = count * stride;
        unsigned cdda = cdda_mask(source, first, count);
        size_t baseline = has_video || has_audio
            ? compress_hunk(encoded, raw, count, stride, cdda, options->zstd_level) : 0;
        if (has_audio) {
            audio_before += baseline;
            memcpy(audio_only, raw, n);
            blank_audio(audio, first, count, stride, audio_only);
            audio_after += compress_hunk(encoded, audio_only, count, stride, cdda, options->zstd_level);
            if (!has_video)
                memcpy(raw, audio_only, n);
        }
        if (has_video) {
            video_before += baseline;
            for (unsigned j = 0; j < count; ++j) {
                if (video->slot[first + j] != PSXRC_NO_INDEX) {
                    uint8_t *sector = raw + j * stride;
                    str_header header;
                    if (!str_parse(sector, &header))
                        goto done;
                    str_make_template(sector, &header);
                }
            }
            video_after += compress_hunk(encoded, raw, count, stride, cdda, options->zstd_level);
            if (has_audio)
                blank_audio(audio, first, count, stride, raw);
        }
        size_t size = compress_hunk(encoded, raw, count, stride, cdda, options->zstd_level);
        put_record(p, offset, (uint32_t)size, av_adler32_update(1, encoded, size));
        if (fwrite(encoded, 1, size, file) != size)
            goto done;
        offset += size;
    }
    if (video->groups && video_after * 100 >= video_before * (100 - options->min_saving)) {
        fputs("STR preserved losslessly: transformed hunks and index missed the savings threshold.\n", stderr);
        result -= 1;
    }
    /* The per-stream check already applies the percentage to audio bytes.
       Here include neighboring data sectors and require net container savings. */
    if (audio->groups && audio_after >= audio_before) {
        fputs("Audio preserved losslessly: transformed hunks and index would enlarge the image.\n", stderr);
        result -= 2;
    }
    if (result || !write_media(file, video->data, video->group, video->groups, video->slot,
                               source->sectors, video->size, &p, &offset) ||
        !write_media(file, audio->data, audio->group, audio->groups, audio->slot,
                     source->sectors, audio->size, &p, &offset))
        goto done;
    if (sbi_bytes)
        memcpy(p, source->sbi, sbi_bytes);
    uint8_t header[PSXRC_HEADER_BYTES] = { 0 };
    memcpy(header, PSXRC_MAGIC, 8);
    write32(header + 8, PSXRC_VERSION);
    unsigned flags = source->flags | PSXRC_FLAG_FILTERS;
    if (video->groups && options->mdec_hints)
        flags |= PSXRC_FLAG_MDEC_HINTS;
    write32(header + 12, flags);
    write32(header + 16, source->sectors);
    write32(header + 20, source->track_count);
    write32(header + 24, hunks);
    write32(header + 28, video->sectors);
    write32(header + 32, video->groups);
    write32(header + 36, av_adler32_update(1, index, index_bytes));
    write32(header + 40, audio->sectors);
    write32(header + 44, audio->groups);
    result = file_seek(file, 0) && fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
             fwrite(index, 1, index_bytes, file) == index_bytes && fflush(file) == 0;
done:
    free(index);
    return result;
}

static int verify_contents(FILE *file, disc_source *source, const video_plan *video, const audio_plan *audio)
{
    uint64_t size;
    psxrc *image = file_size(file, &size) ? psxrc_open(file_read_at, file, size) : NULL;
    if (!image)
        return 0;
    int result = 1;
    size_t sbi_bytes;
    const uint8_t *sbi = psxrc_sbi_bitmap(image, &sbi_bytes);
    if (source->sbi)
        result = sbi && sbi_bytes == (source->sectors + 7) / 8 &&
                 !memcmp(sbi, source->sbi, sbi_bytes);
    for (uint32_t lba = 0; lba < source->sectors && result; ++lba) {
        uint8_t actual[PSXRC_SECTOR_BYTES], subchannel[PSXRC_SUB_BYTES];
        uint8_t expected[PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES];
        result = source_read(source, lba, expected) && psxrc_read_sector(image, lba, actual);
        if (result && (!video->groups || video->slot[lba] == PSXRC_NO_INDEX) &&
            (!audio->groups || audio->slot[lba] == PSXRC_NO_INDEX))
            result = !memcmp(actual, expected, sizeof(actual));
        int sub = psxrc_read_subchannel(image, lba, subchannel);
        if (source->flags & PSXRC_FLAG_SUBCHANNEL)
            result = result && sub == expected[PSXRC_SECTOR_BYTES] &&
                      (!sub || !memcmp(subchannel, expected + PSXRC_SECTOR_BYTES + 1,
                                      PSXRC_SUB_BYTES));
    }
    psxrc_close(image);
    return result;
}

int write_image(disc_source *source, const char *path, const convert_options *options)
{
    video_plan video = { 0 };
    audio_plan audio = { 0 };
    char *temp = NULL, *spool = NULL, *audio_spool = NULL;
    FILE *file = NULL;
    int result = 0;
    if (options->count || options->auto_video) {
        fputs("Scanning and encoding eligible movies...\n", stderr);
        video.data = file_create_temp(path, &spool);
        video.slot = malloc(source->sectors * sizeof(*video.slot));
        video.group = malloc(source->sectors * sizeof(*video.group));
        if (!video.data || !video.slot || !video.group)
            goto done;
        memset(video.slot, 0xff, source->sectors * sizeof(*video.slot));
        if (options->count ? !try_video(source, &video, options, options->first, options->count)
                           : !discover_video(source, &video, options))
            goto done;
    }
    if (options->cdda || options->xa) {
        fputs("Scanning and encoding eligible audio...\n", stderr);
        audio.data = file_create_temp(path, &audio_spool);
        audio.slot = malloc(source->sectors * sizeof(*audio.slot));
        audio.group = malloc(source->sectors * sizeof(*audio.group));
        audio.xa = calloc(source->sectors, 1);
        if (!audio.data || !audio.slot || !audio.group || !audio.xa)
            goto done;
        memset(audio.slot, 0xff, source->sectors * sizeof(*audio.slot));
        if (!audio_plan_build(source, &audio, options))
            goto done;
    }
    int written;
    fputs("Compressing disc hunks...\n", stderr);
    do {
        file = file_create_temp(path, &temp);
        if (!file)
            goto done;
        written = write_contents(file, source, &video, &audio, options);
        if (written < 0) {
            fclose(file);
            file = NULL;
            file_remove(temp);
            free(temp);
            temp = NULL;
            if (-written & 1)
                video.groups = video.sectors = 0;
            if (-written & 2)
                audio.groups = audio.sectors = 0;
        }
    } while (written < 0);
    if (!written)
        goto done;
    fputs("Verifying every sector...\n", stderr);
    if (!verify_contents(file, source, &video, &audio))
        goto done;
    int closed = fclose(file);
    file = NULL;
    if (closed != 0 || !file_publish(temp, path))
        goto done;
    printf("Wrote %u sectors, %u STR and %u audio sectors reconstructed on read.\n",
           source->sectors, video.sectors, audio.sectors);
    result = 1;
done:
    if (file)
        fclose(file);
    if (temp) {
        if (!result)
            file_remove(temp);
        free(temp);
    }
    if (video.data)
        fclose(video.data);
    if (spool) {
        file_remove(spool);
        free(spool);
    }
    if (audio.data)
        fclose(audio.data);
    if (audio_spool) {
        file_remove(audio_spool);
        free(audio_spool);
    }
    free(video.group);
    free(video.slot);
    free(audio.group);
    free(audio.slot);
    free(audio.xa);
    return result;
}
