#include "source.h"
#include "file_io.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int same_word(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static unsigned bcd(unsigned n) { return n / 10 * 16 + n % 10; }

static unsigned from_bcd(unsigned n) { return n / 16 * 10 + n % 16; }

static int import_sbi(disc_source *source, FILE *file)
{
    uint8_t bytes[10];
    if (fread(bytes, 1, 4, file) != 4 || memcmp(bytes, "SBI\0", 4))
        return 0;
    source->sbi = calloc((source->sectors + 7) / 8, 1);
    if (!source->sbi)
        return 0;
    for (;;) {
        size_t n = fread(bytes, 1, 4, file);
        if (!n && feof(file))
            break;
        if (n != 4 || bytes[0] > 0x99 || bytes[1] > 0x59 || bytes[2] > 0x74 ||
            (bytes[0] & 15) > 9 || (bytes[1] & 15) > 9 || (bytes[2] & 15) > 9 ||
            bytes[3] < 1 || bytes[3] > 3)
            return 0;
        unsigned msf = from_bcd(bytes[0]) * 4500 + from_bcd(bytes[1]) * 75 + from_bcd(bytes[2]);
        if (msf < 150 || msf - 150 >= source->sectors)
            return 0;
        unsigned lba = msf - 150;
        source->sbi[lba / 8] |= (uint8_t)(1 << (lba % 8));
        size_t payload = bytes[3] == 1 ? 10 : 3;
        if (fread(bytes, 1, payload, file) != payload)
            return 0;
    }
    source->flags |= PSXRC_FLAG_SBI;
    return 1;
}

static int parse_time(const char *text, uint32_t *sector)
{
    unsigned min, sec, frac;
    int used = 0;
    if (sscanf(text, "%2u:%2u:%2u %n", &min, &sec, &frac, &used) != 3 ||
        text[used] || min > 99 || sec >= 60 || frac >= 75)
        return 0;
    *sector = (min * 60 + sec) * 75 + frac;
    return 1;
}

static size_t directory_length(const char *path)
{
    size_t length = 0;
    for (size_t i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\')
            length = i + 1;
    return length;
}

static FILE *open_relative(const char *cue, const char *name)
{
    if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':'))
        return fopen(name, "rb");
    size_t directory = directory_length(cue);
    char *path = malloc(directory + strlen(name) + 1);
    if (!path)
        return NULL;
    memcpy(path, cue, directory);
    strcpy(path + directory, name);
    FILE *file = fopen(path, "rb");
    free(path);
    return file;
}

static int finalize_cue(disc_source *source, const uint32_t *file_lengths)
{
    unsigned file_index = 0;
    for (unsigned i = 0; i < source->track_count; ++i) {
        psxrc_track *t = &source->track[i];
        source_part *part = &source->part[i];
        while (file_index < source->file_count && source->files[file_index] != part->file)
            ++file_index;
        if (file_index == source->file_count || t->index1 == PSXRC_NO_INDEX)
            return 0;
        uint32_t first = t->index0 == PSXRC_NO_INDEX ? t->index1 : t->index0;
        if ((!i || source->part[i - 1].file != part->file) && first)
            return 0;
        uint32_t end = file_lengths[file_index];
        if (i + 1 < source->track_count && source->part[i + 1].file == part->file) {
            const psxrc_track *next = &source->track[i + 1];
            end = next->index0 == PSXRC_NO_INDEX ? next->index1 : next->index0;
        }
        if (first > t->index1 || t->index1 >= end || end > file_lengths[file_index] ||
            part->gap > PSXRC_MAX_SECTORS - source->sectors ||
            end - first > PSXRC_MAX_SECTORS - source->sectors - part->gap)
            return 0;
        part->sector = first;
        t->index0 = source->sectors;
        t->index1 = source->sectors + part->gap + t->index1 - first;
        source->sectors += part->gap + end - first;
        t->end = source->sectors;
    }
    return source->track_count != 0 && file_index + 1 == source->file_count;
}

static int open_cue(disc_source *source, const char *path)
{
    FILE *cue = fopen(path, "rb");
    if (!cue)
        return 0;
    char line[4096], keyword[16], value[32];
    unsigned count_before_file = 0;
    uint32_t file_lengths[PSXRC_MAX_TRACKS];
    FILE *binary = NULL;
    psxrc_track *track = NULL;
    source_part *part = NULL;
    int valid = 0;
    while (fgets(line, sizeof(line), cue)) {
        if (!strchr(line, '\n') && !feof(cue))
            goto done;
        int used = 0;
        if (sscanf(line, " %15s %n", keyword, &used) != 1)
            continue;
        const char *fields = line + used;
        if (same_word(keyword, "FILE")) {
            char name[4096];
            if (source->file_count == PSXRC_MAX_TRACKS ||
                (binary && count_before_file == source->track_count))
                goto done;
            if (sscanf(fields, "\"%4095[^\"]\" %31s", name, value) != 2 &&
                sscanf(fields, "%4095s %31s", name, value) != 2)
                goto done;
            if (!same_word(value, "BINARY"))
                goto done;
            binary = open_relative(path, name);
            if (!binary)
                goto done;
            unsigned index = source->file_count++;
            source->files[index] = binary;
            uint64_t size;
            if (!file_size(binary, &size) || !size || size % PSXRC_SECTOR_BYTES ||
                size / PSXRC_SECTOR_BYTES > PSXRC_MAX_SECTORS)
                goto done;
            file_lengths[index] = (uint32_t)(size / PSXRC_SECTOR_BYTES);
            count_before_file = source->track_count;
            track = NULL;
        } else if (same_word(keyword, "TRACK")) {
            unsigned number;
            if (!binary || source->track_count == PSXRC_MAX_TRACKS ||
                sscanf(fields, "%2u %31s", &number, value) != 2 ||
                number != source->track_count + 1)
                goto done;
            enum psxrc_mode mode;
            if (same_word(value, "MODE1/2352") || same_word(value, "MODE2/2352"))
                mode = PSXRC_RAW_DATA;
            else if (same_word(value, "AUDIO"))
                mode = PSXRC_CDDA;
            else
                goto done;
            track = &source->track[source->track_count];
            part = &source->part[source->track_count++];
            *track = (psxrc_track){ PSXRC_NO_INDEX, PSXRC_NO_INDEX, 0, mode };
            part->file = binary;
        } else if (same_word(keyword, "INDEX")) {
            unsigned index;
            used = 0;
            if (!track || sscanf(fields, "%2u %n", &index, &used) != 1 || index > 1)
                goto done;
            uint32_t *at = index ? &track->index1 : &track->index0;
            if (*at != PSXRC_NO_INDEX || !parse_time(fields + used, at))
                goto done;
        } else if (same_word(keyword, "PREGAP")) {
            if (!track || part->gap || !parse_time(fields, &part->gap))
                goto done;
        } else if (!same_word(keyword, "REM") && !same_word(keyword, "TITLE") &&
                   !same_word(keyword, "PERFORMER")) {
            goto done;
        }
    }
    valid = !ferror(cue) && finalize_cue(source, file_lengths);
done:
    fclose(cue);
    return valid;
}

static int open_flat(disc_source *source, const char *path, unsigned sector_bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    source->files[source->file_count++] = file;
    uint64_t size;
    if (!file_size(file, &size) || !size)
        return 0;
    if (!sector_bytes) {
        static const uint8_t sync[] = { 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0 };
        uint8_t header[16];
        sector_bytes = size % PSXRC_SECTOR_BYTES == 0 &&
                       file_read_at(file, 0, header, sizeof(header)) &&
                       !memcmp(header, sync, sizeof(sync)) && (header[15] == 1 || header[15] == 2)
                       ? PSXRC_SECTOR_BYTES : 2048;
    }
    if (size % sector_bytes ||
        size / sector_bytes > PSXRC_MAX_SECTORS)
        return 0;
    uint32_t sectors = (uint32_t)(size / sector_bytes);
    source->track_count = 1;
    source->sectors = sectors;
    source->track[0] = (psxrc_track){ 0, 0, sectors,
                                     sector_bytes == 2048 ? PSXRC_COOKED_MODE1 : PSXRC_RAW_DATA };
    source->part[0].file = file;
    return 1;
}

int source_open(disc_source *source, const char *path)
{
    memset(source, 0, sizeof(*source));
    const char *ext = strrchr(path + directory_length(path), '.');
    if (!ext)
        return 0;
    int valid = same_word(ext, ".cue") ? open_cue(source, path)
              : same_word(ext, ".iso") ? open_flat(source, path, 0)
              : same_word(ext, ".bin") ? open_flat(source, path, PSXRC_SECTOR_BYTES)
#ifdef PSXRC_HAVE_CHD
              : same_word(ext, ".chd") ? source_chd_open(source, path)
#endif
              : 0;
    if (valid) {
        size_t n = (size_t)(ext - path);
        char *sub = malloc(n + 5);
        if (!sub)
            return 0;
        memcpy(sub, path, n);
        memcpy(sub + n, ".sub", 5);
        source->sub = fopen(sub, "rb");
        memcpy(sub + n, ".sbi", 5);
        FILE *sbi = fopen(sub, "rb");
        free(sub);
        if (sbi) {
            valid = import_sbi(source, sbi);
            fclose(sbi);
            if (!valid)
                return 0;
        }
        uint64_t size;
        if (source->sub && (!file_size(source->sub, &size) ||
                            size != (uint64_t)source->sectors * PSXRC_SUB_BYTES))
            return 0;
        if (source->sub)
            source->flags |= PSXRC_FLAG_SUBCHANNEL;
    }
    return valid;
}

void source_close(disc_source *source)
{
#ifdef PSXRC_HAVE_CHD
    source_chd_close(source);
#endif
    for (unsigned i = 0; i < source->file_count; ++i)
        fclose(source->files[i]);
    if (source->sub)
        fclose(source->sub);
    free(source->sbi);
}

void source_cooked_header(uint8_t *bytes, uint32_t lba)
{
    uint32_t address = lba + 150;
    memset(bytes, 0, PSXRC_SECTOR_BYTES);
    bytes[12] = (uint8_t)bcd(address / 4500);
    bytes[13] = (uint8_t)bcd(address / 75 % 60);
    bytes[14] = (uint8_t)bcd(address % 75);
    bytes[15] = 1;
}

int source_read(disc_source *source, uint32_t lba, uint8_t *bytes)
{
    if (lba >= source->sectors)
        return 0;
    unsigned i = 0;
    while (lba >= source->track[i].end)
        ++i;
    const psxrc_track *track = &source->track[i];
    source_part *part = &source->part[i];
    uint32_t position = lba - track->index0;
    int result = 1;
    if (source->flags & PSXRC_FLAG_SUBCHANNEL)
        memset(bytes + PSXRC_SECTOR_BYTES, 0, PSXRC_SUB_RECORD_BYTES);
#ifdef PSXRC_HAVE_CHD
    if (source->chd) {
        result = source_chd_read(source, i, lba, bytes);
    } else
#endif
    if (position < part->gap) {
        memset(bytes, 0, PSXRC_SECTOR_BYTES);
    } else if (track->mode == PSXRC_COOKED_MODE1) {
        source_cooked_header(bytes, lba);
        result = file_read_at(part->file, (uint64_t)(part->sector + position - part->gap) * 2048,
                              bytes + 24, 2048);
    } else {
        result = file_read_at(part->file, (uint64_t)(part->sector + position - part->gap) *
                              PSXRC_SECTOR_BYTES, bytes, PSXRC_SECTOR_BYTES);
    }
    if (source->sub) {
        bytes[PSXRC_SECTOR_BYTES] = 1;
        result = result && file_read_at(source->sub, (uint64_t)lba * PSXRC_SUB_BYTES,
                                        bytes + PSXRC_SECTOR_BYTES + 1, PSXRC_SUB_BYTES);
    }
    return result;
}
