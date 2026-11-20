#include "format.h"
#include "filters.h"
#include "bytes.h"
#include "codec.h"
#include "str.h"
#include "audio.h"
#include "xa.h"
#include <libavutil/adler32.h>
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

#define HUNK_CACHE_SIZE 4
#define GROUP_CACHE_SIZE 2

typedef struct {
    uint32_t lba, group;
    unsigned frame;
} media_mapping;

typedef struct {
    uint32_t index;
    uint8_t data[PSXRC_FILTER_BYTES + PSXRC_HUNK_SECTORS * (PSXRC_SECTOR_BYTES + PSXRC_SUB_RECORD_BYTES)];
} hunk_cache;

typedef struct {
    unsigned quant, version, words;
    uint32_t packet_offset, packet_size, capacity, expected_size, checksum;
} frame_spec;

typedef struct {
    uint32_t index;
    uint8_t frame[PSXRC_GROUP_FRAMES][STR_MAX_FRAME_BYTES];
    size_t length[PSXRC_GROUP_FRAMES];
    frame_spec spec[PSXRC_GROUP_FRAMES];
    uint8_t *packets;
    unsigned width, height, count;
} group_cache;

typedef struct {
    uint32_t index;
    unsigned coding, units;
    enum audio_kind kind;
    uint8_t data[AUDIO_MAX_UNITS * AUDIO_UNIT_BYTES];
} audio_cache;

struct psxrc {
    psxrc_read_at read_at;
    void *context;
    uint32_t sectors, hunks, videos, groups, tracks, audios, audio_groups;
    unsigned flags, sector_bytes, filter_bytes;
    psxrc_track track[PSXRC_MAX_TRACKS];
    psxrc_record *hunk, *group, *audio_group;
    media_mapping *video, *audio;
    uint8_t *sbi;
    hunk_cache cached_hunks[HUNK_CACHE_SIZE];
    group_cache cached_groups[GROUP_CACHE_SIZE];
    audio_cache cached_audio[GROUP_CACHE_SIZE];
    unsigned next_hunk, next_group, next_audio;
    uint32_t decoder_group;
    unsigned decoder_next;
    ZSTD_DCtx *zstd;
    AVCodecContext *decoder;
    AVFrame *picture;
};

static uint32_t hunk_bytes(const psxrc *image, uint32_t i)
{
    uint32_t sectors = image->sectors - i * PSXRC_HUNK_SECTORS;
    if (sectors > PSXRC_HUNK_SECTORS)
        sectors = PSXRC_HUNK_SECTORS;
    return sectors * image->sector_bytes + image->filter_bytes;
}

static int load_records(psxrc_record *records, const uint8_t *p, uint32_t count,
                        uint64_t *data_offset, uint64_t size, const psxrc *image, unsigned type)
{
    for (uint32_t i = 0; i < count; ++i, p += PSXRC_RECORD_BYTES) {
        uint32_t limit = type == 2 ? AUDIO_MAX_GROUP
                       : type == 1 ? PSXRC_GROUP_FRAMES * (PSXRC_MAX_PACKET + PSXRC_FRAME_BYTES) + 8
                                   : hunk_bytes(image, i);
        records[i] = (psxrc_record){ read64(p), read32(p + 8), read32(p + 12) };
        if (records[i].offset != *data_offset || !records[i].bytes ||
            records[i].bytes > limit || records[i].bytes > size - *data_offset)
            return 0;
        *data_offset += records[i].bytes;
    }
    return 1;
}

static int load_mapping(media_mapping *map, const uint8_t *p, uint32_t count,
                        uint32_t sectors, uint32_t groups, unsigned slots)
{
    for (uint32_t i = 0; i < count; ++i, p += PSXRC_MAP_BYTES) {
        uint32_t slot = read32(p + 4);
        map[i] = (media_mapping){ read32(p), slot / slots, slot % slots };
        if (map[i].lba >= sectors || map[i].group >= groups ||
            (i && map[i - 1].lba >= map[i].lba))
            return 0;
    }
    return 1;
}

psxrc *psxrc_open(psxrc_read_at read_at, void *context, uint64_t size)
{
    uint8_t header[PSXRC_HEADER_BYTES], *index = NULL;
    if (!read_at || size < sizeof(header) || !read_at(context, 0, header, sizeof(header)) ||
        memcmp(header, PSXRC_MAGIC, 8) || read32(header + 8) != PSXRC_VERSION)
        return NULL;
    psxrc *image = calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    image->read_at = read_at;
    image->context = context;
    image->flags = read32(header + 12);
    image->sectors = read32(header + 16);
    image->tracks = read32(header + 20);
    image->hunks = read32(header + 24);
    image->videos = read32(header + 28);
    image->groups = read32(header + 32);
    image->audios = read32(header + 40);
    image->audio_groups = read32(header + 44);
    image->sector_bytes = PSXRC_SECTOR_BYTES +
        ((image->flags & PSXRC_FLAG_SUBCHANNEL) ? PSXRC_SUB_RECORD_BYTES : 0);
    image->filter_bytes = image->flags & PSXRC_FLAG_FILTERS ? PSXRC_FILTER_BYTES : 0;
    if (!image->sectors || image->sectors > PSXRC_MAX_SECTORS ||
        !image->tracks || image->tracks > PSXRC_MAX_TRACKS ||
        image->flags & ~(PSXRC_FLAG_SUBCHANNEL | PSXRC_FLAG_SBI | PSXRC_FLAG_FILTERS) ||
        image->hunks != (image->sectors + PSXRC_HUNK_SECTORS - 1) / PSXRC_HUNK_SECTORS ||
        image->videos > image->sectors || image->groups > image->videos ||
        image->audios > image->sectors - image->videos || image->audio_groups > image->audios ||
        (!!image->videos != !!image->groups) || (!!image->audios != !!image->audio_groups))
        goto fail;
    size_t sbi_bytes = image->flags & PSXRC_FLAG_SBI ? (image->sectors + 7) / 8 : 0;
    size_t index_bytes = image->tracks * PSXRC_TRACK_BYTES +
        (image->hunks + image->groups + image->audio_groups) * PSXRC_RECORD_BYTES +
        (image->videos + image->audios) * PSXRC_MAP_BYTES + sbi_bytes;
    uint64_t data_offset = sizeof(header) + index_bytes;
    if (data_offset > size)
        goto fail;
    index = malloc(index_bytes);
    image->hunk = malloc(image->hunks * sizeof(*image->hunk));
    image->zstd = ZSTD_createDCtx();
    if (!index || !image->hunk || !image->zstd ||
        ZSTD_isError(ZSTD_DCtx_setParameter(image->zstd, ZSTD_d_windowLogMax, 17)) ||
        !read_at(context, sizeof(header), index, index_bytes) ||
        av_adler32_update(1, index, index_bytes) != read32(header + 36))
        goto fail;
    const uint8_t *p = index;
    for (uint32_t i = 0; i < image->tracks; ++i, p += PSXRC_TRACK_BYTES) {
        psxrc_track *t = &image->track[i];
        uint32_t prev_end = i ? image->track[i - 1].end : 0;
        uint32_t mode = read32(p + 12);
        *t = (psxrc_track){ read32(p), read32(p + 4), read32(p + 8), (enum psxrc_mode)mode };
        if (mode > PSXRC_CDDA || t->index0 != prev_end || t->index1 < t->index0 ||
            t->end <= t->index1 || t->end > image->sectors)
            goto fail;
    }
    if (image->track[image->tracks - 1].end != image->sectors ||
        !load_records(image->hunk, p, image->hunks, &data_offset, size, image, 0))
        goto fail;
    p += image->hunks * PSXRC_RECORD_BYTES;
    if (image->groups) {
        image->group = malloc(image->groups * sizeof(*image->group));
        image->video = malloc(image->videos * sizeof(*image->video));
        image->picture = av_frame_alloc();
        if (!image->group || !image->video || !image->picture ||
            !load_records(image->group, p, image->groups, &data_offset, size, image, 1))
            goto fail;
        p += image->groups * PSXRC_RECORD_BYTES;
        if (!load_mapping(image->video, p, image->videos, image->sectors, image->groups, PSXRC_GROUP_FRAMES))
            goto fail;
        p += image->videos * PSXRC_MAP_BYTES;
    }
    if (image->audio_groups) {
        image->audio_group = malloc(image->audio_groups * sizeof(*image->audio_group));
        image->audio = malloc(image->audios * sizeof(*image->audio));
        if (!image->audio_group || !image->audio ||
            !load_records(image->audio_group, p, image->audio_groups, &data_offset, size, image, 2))
            goto fail;
        p += image->audio_groups * PSXRC_RECORD_BYTES;
        if (!load_mapping(image->audio, p, image->audios, image->sectors, image->audio_groups, AUDIO_MAX_UNITS))
            goto fail;
        p += image->audios * PSXRC_MAP_BYTES;
        uint32_t v = 0;
        for (uint32_t a = 0; a < image->audios && v < image->videos; ++a) {
            while (v < image->videos && image->video[v].lba < image->audio[a].lba)
                ++v;
            if (v < image->videos && image->video[v].lba == image->audio[a].lba)
                goto fail;
        }
    }
    if (data_offset != size)
        goto fail;
    if (sbi_bytes) {
        image->sbi = malloc(sbi_bytes);
        if (!image->sbi)
            goto fail;
        memcpy(image->sbi, p, sbi_bytes);
    }
    for (unsigned i = 0; i < HUNK_CACHE_SIZE; ++i)
        image->cached_hunks[i].index = PSXRC_NO_INDEX;
    for (unsigned i = 0; i < GROUP_CACHE_SIZE; ++i) {
        image->cached_groups[i].index = PSXRC_NO_INDEX;
        image->cached_audio[i].index = PSXRC_NO_INDEX;
    }
    image->decoder_group = PSXRC_NO_INDEX;
    free(index);
    return image;
fail:
    free(index);
    psxrc_close(image);
    return NULL;
}

void psxrc_close(psxrc *image)
{
    if (!image)
        return;
    avcodec_free_context(&image->decoder);
    av_frame_free(&image->picture);
    ZSTD_freeDCtx(image->zstd);
    free(image->hunk);
    free(image->group);
    free(image->video);
    free(image->audio_group);
    free(image->audio);
    free(image->sbi);
    for (unsigned i = 0; i < GROUP_CACHE_SIZE; ++i)
        free(image->cached_groups[i].packets);
    free(image);
}

unsigned psxrc_track_count(const psxrc *image) { return image->tracks; }
uint32_t psxrc_sector_count(const psxrc *image) { return image->sectors; }
uint32_t psxrc_video_sector_count(const psxrc *image) { return image->videos; }
uint32_t psxrc_audio_sector_count(const psxrc *image) { return image->audios; }
int psxrc_has_subchannel(const psxrc *image) { return !!(image->flags & PSXRC_FLAG_SUBCHANNEL); }

const uint8_t *psxrc_sbi_bitmap(const psxrc *image, size_t *bytes)
{
    *bytes = image->sbi ? (image->sectors + 7) / 8 : 0;
    return image->sbi;
}

const psxrc_track *psxrc_get_track(const psxrc *image, unsigned number)
{
    return number && number <= image->tracks ? &image->track[number - 1] : NULL;
}

static const uint8_t *sector_from_hunk(psxrc *image, hunk_cache *cache, unsigned index,
                                      int restore, int *omitted_ecc)
{
    if (omitted_ecc)
        *omitted_ecc = image->filter_bytes && (cache->data[0] & (1u << index));
    if (restore && image->filter_bytes && !unfilter_sector(cache->data, index, image->sector_bytes))
        return NULL;
    return cache->data + index * image->sector_bytes + image->filter_bytes;
}

static const uint8_t *read_template(psxrc *image, uint32_t lba, int restore, int *omitted_ecc)
{
    if (lba >= image->sectors)
        return NULL;
    uint32_t i = lba / PSXRC_HUNK_SECTORS;
    unsigned index = lba % PSXRC_HUNK_SECTORS;
    for (unsigned slot = 0; slot < HUNK_CACHE_SIZE; ++slot)
        if (image->cached_hunks[slot].index == i)
            return sector_from_hunk(image, &image->cached_hunks[slot], index, restore, omitted_ecc);
    hunk_cache *cache = &image->cached_hunks[image->next_hunk++ % HUNK_CACHE_SIZE];
    cache->index = PSXRC_NO_INDEX;
    psxrc_record *r = &image->hunk[i];
    size_t bytes = hunk_bytes(image, i);
    uint8_t compressed[sizeof(cache->data)];
    uint8_t *dst = r->bytes == bytes ? cache->data : compressed;
    if (!image->read_at(image->context, r->offset, dst, r->bytes) ||
        av_adler32_update(1, dst, r->bytes) != r->checksum ||
        (dst == compressed && ZSTD_decompressDCtx(image->zstd, cache->data, bytes,
                                                   compressed, r->bytes) != bytes))
        return NULL;
    if (image->filter_bytes &&
        !filter_masks_valid(cache->data, (bytes - image->filter_bytes) / image->sector_bytes))
        return NULL;
    cache->index = i;
    return sector_from_hunk(image, cache, index, restore, omitted_ecc);
}

static group_cache *read_group(psxrc *image, uint32_t group)
{
    for (unsigned i = 0; i < GROUP_CACHE_SIZE; ++i)
        if (image->cached_groups[i].index == group)
            return &image->cached_groups[i];
    group_cache *cache = &image->cached_groups[image->next_group++ % GROUP_CACHE_SIZE];
    cache->index = PSXRC_NO_INDEX;
    memset(cache->length, 0, sizeof(cache->length));
    const psxrc_record *record = &image->group[group];
    uint8_t *packets = realloc(cache->packets, record->bytes);
    if (!packets)
        return NULL;
    cache->packets = packets;
    const uint8_t *p = packets;
    if (record->bytes < 8 ||
        !image->read_at(image->context, record->offset, packets, record->bytes) ||
        av_adler32_update(1, p, record->bytes) != record->checksum)
        return NULL;
    unsigned count = read16(p), width = read16(p + 2), height = read16(p + 4);
    if (!count || count > PSXRC_GROUP_FRAMES || !width || width > 1024 || width % 2 ||
        !height || height > 1024 || height % 2 || read16(p + 6))
        return NULL;
    const uint8_t *end = p + record->bytes;
    p += 8;
    for (unsigned i = 0; i < count; ++i) {
        if ((size_t)(end - p) < PSXRC_FRAME_BYTES)
            return NULL;
        frame_spec *f = &cache->spec[i];
        *f = (frame_spec){ p[0], p[1], read16(p + 2), (uint32_t)(p - packets) + PSXRC_FRAME_BYTES,
                           read32(p + 4), read32(p + 8), read32(p + 12), read32(p + 16) };
        p += PSXRC_FRAME_BYTES;
        if (!f->packet_size || f->packet_size > PSXRC_MAX_PACKET ||
            f->packet_size > (size_t)(end - p) || f->capacity > STR_MAX_FRAME_BYTES ||
            !f->quant || f->quant > 63 || (f->version != 2 && f->version != 3))
            return NULL;
        p += f->packet_size;
    }
    if (p != end)
        return NULL;
    cache->width = width;
    cache->height = height;
    cache->count = count;
    cache->index = group;
    return cache;
}

static int read_frame(psxrc *image, group_cache *cache, unsigned frame)
{
    if (frame >= cache->count)
        return 0;
    if (cache->length[frame])
        return 1;
    if (!image->decoder)
        image->decoder = codec_decoder(AV_CODEC_ID_H264, 0, 0);
    if (!image->decoder)
        return 0;
    if (image->decoder_group != cache->index || image->decoder_next > frame) {
        avcodec_flush_buffers(image->decoder);
        image->decoder_group = cache->index;
        image->decoder_next = 0;
    }
    while (image->decoder_next <= frame) {
        const frame_spec *f = &cache->spec[image->decoder_next++];
        if (!codec_decode(image->decoder, cache->packets + f->packet_offset,
                          f->packet_size, image->picture)) {
            image->decoder_group = PSXRC_NO_INDEX;
            return 0;
        }
    }
    bs_picture pixels;
    if (!codec_picture(image->picture, cache->width, cache->height, &pixels))
        return 0;
    const frame_spec *f = &cache->spec[frame];
    size_t size = bs_encode(&pixels, f->version, f->quant, f->words, cache->frame[frame], f->capacity);
    if (!size || size != f->expected_size || av_adler32_update(1, cache->frame[frame], size) != f->checksum)
        return 0;
    cache->length[frame] = size;
    return 1;
}

static const media_mapping *find_mapping(const media_mapping *map, uint32_t count, uint32_t lba)
{
    uint32_t first = 0, last = count;
    while (first < last) {
        uint32_t mid = first + (last - first) / 2;
        if (map[mid].lba < lba)
            first = mid + 1;
        else
            last = mid;
    }
    return first < count && map[first].lba == lba ? &map[first] : NULL;
}

static const audio_cache *read_audio(psxrc *image, uint32_t group)
{
    for (unsigned i = 0; i < GROUP_CACHE_SIZE; ++i)
        if (image->cached_audio[i].index == group)
            return &image->cached_audio[i];
    audio_cache *cache = &image->cached_audio[image->next_audio++ % GROUP_CACHE_SIZE];
    cache->index = PSXRC_NO_INDEX;
    const psxrc_record *r = &image->audio_group[group];
    uint8_t compressed[AUDIO_MAX_GROUP];
    if (!image->read_at(image->context, r->offset, compressed, r->bytes) ||
        av_adler32_update(1, compressed, r->bytes) != r->checksum ||
        !audio_unpack(compressed, r->bytes, cache->data, &cache->kind, &cache->coding, &cache->units))
        return NULL;
    cache->index = group;
    return cache;
}

static int overlay_audio(psxrc *image, const media_mapping *map, uint8_t *output)
{
    const audio_cache *cache = read_audio(image, map->group);
    if (!cache || map->frame >= cache->units)
        return 0;
    const uint8_t *audio = cache->data + map->frame * AUDIO_UNIT_BYTES;
    if (cache->kind == AUDIO_CDDA) {
        for (unsigned i = 0; i < image->tracks; ++i) {
            const psxrc_track *t = &image->track[i];
            if (map->lba >= t->index1 && map->lba < t->end && t->mode == PSXRC_CDDA) {
                memcpy(output, audio, PSXRC_SECTOR_BYTES);
                return 1;
            }
        }
        return 0;
    }
    if (!xa_header(output) || output[19] != cache->coding)
        return 0;
    memcpy(output + 24, audio, XA_DATA_BYTES);
    if (read32(output + 2348))
        write32(output + 2348, str_edc(output + 16, 2332));
    return 1;
}

int psxrc_read_sector(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SECTOR_BYTES])
{
    const media_mapping *map = find_mapping(image->video, image->videos, lba);
    const media_mapping *audio = map ? NULL : find_mapping(image->audio, image->audios, lba);
    int omitted_ecc = 0;
    const uint8_t *data = read_template(image, lba, !map && !audio, &omitted_ecc);
    if (!data)
        return 0;
    memcpy(output, data, PSXRC_SECTOR_BYTES);
    if (!map) {
        if (audio)
            return overlay_audio(image, audio, output);
        return 1;
    }
    group_cache *cache = read_group(image, map->group);
    str_header h;
    int valid = omitted_ecc ? !(data[18] & 0x20) && str_parse_template(data, &h) : str_parse(data, &h);
    if (!cache || !valid || !read_frame(image, cache, map->frame) ||
        cache->length[map->frame] > h.length || cache->width != h.width || cache->height != h.height ||
        read16(cache->frame[map->frame]) > h.words || read16(cache->frame[map->frame] + 6) != h.version)
        return 0;
    str_replace(output, &h, cache->frame[map->frame], cache->length[map->frame]);
    return 1;
}

int psxrc_read_subchannel(psxrc *image, uint32_t lba, uint8_t output[PSXRC_SUB_BYTES])
{
    if (!psxrc_has_subchannel(image))
        return 0;
    const uint8_t *data = read_template(image, lba, 0, NULL);
    if (!data || data[PSXRC_SECTOR_BYTES] != 1)
        return 0;
    memcpy(output, data + PSXRC_SECTOR_BYTES + 1, PSXRC_SUB_BYTES);
    return 1;
}
