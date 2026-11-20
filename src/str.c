#include "str.h"
#include "bytes.h"
#include <string.h>

enum {
    SUBHEADER = 16,
    STR_HEADER = 24,
    PAYLOAD = 56,
    FORM1_EDC = 2072,
    FORM1_ECC = 2076,
    FORM2_EDC = 2348
};

uint32_t str_edc(const uint8_t *data, size_t size)
{
    uint32_t table[256];
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (value & 1 ? UINT32_C(0xd8018001) : 0);
        table[i] = value;
    }
    uint32_t edc = 0;
    while (size--)
        edc = (edc >> 8) ^ table[(edc ^ *data++) & 255];
    return edc;
}

/* Mode 2 Form 1 ECC treats the four address and mode bytes as zero. */
static void ecc_rows(const uint8_t *source, unsigned majors, unsigned minors, unsigned stride,
                      unsigned step, const uint8_t *forward, const uint8_t *backward, uint8_t *out)
{
    unsigned size = majors * minors;
    for (unsigned major = 0; major < majors; ++major) {
        unsigned index = (major / 2) * stride + major % 2;
        uint8_t a = 0, b = 0;
        for (unsigned minor = 0; minor < minors; ++minor) {
            uint8_t value = index < 4 ? 0 : source[index];
            a = forward[a ^ value];
            b ^= value;
            index += step;
            if (index >= size)
                index -= size;
        }
        a = backward[forward[a] ^ b];
        out[major] = a;
        out[major + majors] = a ^ b;
    }
}

void str_write_integrity(uint8_t sector[STR_SECTOR_BYTES])
{
    if (sector[SUBHEADER + 2] & 0x20) {
        write32(sector + FORM2_EDC, str_edc(sector + SUBHEADER, FORM2_EDC - SUBHEADER));
        return;
    }
    uint8_t forward[256], backward[256];
    for (unsigned i = 0; i < 256; ++i) {
        unsigned doubled = i << 1;
        if (i & 128)
            doubled ^= 0x11d;
        forward[i] = (uint8_t)doubled;
        backward[i ^ doubled] = (uint8_t)i;
    }
    write32(sector + FORM1_EDC, str_edc(sector + SUBHEADER, FORM1_EDC - SUBHEADER));
    ecc_rows(sector + 12, 86, 24, 2, 86, forward, backward, sector + FORM1_ECC);
    ecc_rows(sector + 12, 52, 43, 86, 88, forward, backward, sector + FORM1_ECC + 172);
}

int str_form1_integrity(const uint8_t sector[STR_SECTOR_BYTES])
{
    static const uint8_t sync[] = {
        0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0
    };
    if (sector[15] != 2 || sector[SUBHEADER + 2] & 0x20 ||
        memcmp(sector, sync, sizeof(sync)) || memcmp(sector + SUBHEADER, sector + SUBHEADER + 4, 4))
        return 0;
    uint8_t reference[STR_SECTOR_BYTES];
    memcpy(reference, sector, FORM1_EDC);
    str_write_integrity(reference);
    return !memcmp(reference + FORM1_EDC, sector + FORM1_EDC, STR_SECTOR_BYTES - FORM1_EDC);
}

int str_parse_template(const uint8_t sector[STR_SECTOR_BYTES], str_header *header)
{
    static const uint8_t sync[] = {
        0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0
    };
    if (memcmp(sector, sync, sizeof(sync)) || sector[15] != 2 ||
        memcmp(sector + SUBHEADER, sector + SUBHEADER + 4, 4) ||
        sector[SUBHEADER + 1] >= 32)
        return 0;
    unsigned submode = sector[SUBHEADER + 2] & 0x0e;
    if (submode != 0x02 && submode != 0x08)
        return 0;
    const uint8_t *p = sector + STR_HEADER;
    if (read32(p) != UINT32_C(0x80010160) || read32(p + 28) != 0 ||
        read16(p + 22) != 0x3800)
        return 0;
    *header = (str_header) {
        .chunk = read16(p + 4), .chunks = read16(p + 6), .number = read32(p + 8),
        .length = read32(p + 12), .width = read16(p + 16), .height = read16(p + 18),
        .words = read16(p + 20), .quant = read16(p + 24), .version = read16(p + 26),
        .file = sector[SUBHEADER], .channel = sector[SUBHEADER + 1]
    };
    if (!header->chunks || header->chunks > STR_MAX_CHUNKS || header->chunk >= header->chunks ||
        header->length < 12 || header->length % 4 ||
        header->length > header->chunks * STR_CHUNK_BYTES ||
        !header->width || header->width > 1024 || header->width % 2 ||
        !header->height || header->height > 1024 || header->height % 2 ||
        !header->words || header->words % 32 || !header->quant || header->quant > 63 ||
        (header->version != 2 && header->version != 3))
        return 0;
    return 1;
}

int str_parse(const uint8_t sector[STR_SECTOR_BYTES], str_header *header)
{
    if (!str_parse_template(sector, header))
        return 0;
    if (sector[SUBHEADER + 2] & 0x20) {
        uint32_t edc = read32(sector + FORM2_EDC);
        return !edc || edc == str_edc(sector + SUBHEADER, FORM2_EDC - SUBHEADER);
    }
    return str_form1_integrity(sector);
}

int str_same_frame(const str_header *a, const str_header *b)
{
    return a->chunks == b->chunks && a->number == b->number && a->length == b->length &&
           a->width == b->width && a->height == b->height && a->words == b->words &&
           a->quant == b->quant && a->version == b->version &&
           a->file == b->file && a->channel == b->channel;
}

int str_has_signature(const uint8_t sector[STR_SECTOR_BYTES])
{
    return sector[15] == 2 && read32(sector + STR_HEADER) == UINT32_C(0x80010160);
}

void str_make_template(uint8_t sector[STR_SECTOR_BYTES], const str_header *original)
{
    size_t offset = original->chunk * STR_CHUNK_BYTES;
    size_t bytes = offset < original->length ? original->length - offset : 0;
    if (bytes > STR_CHUNK_BYTES)
        bytes = STR_CHUNK_BYTES;
    memset(sector + PAYLOAD, 0, bytes);
    if (!(sector[SUBHEADER + 2] & 0x20) || read32(sector + FORM2_EDC))
        str_write_integrity(sector);
}

void str_replace(uint8_t sector[STR_SECTOR_BYTES], const str_header *original,
                 const uint8_t *frame, size_t length)
{
    size_t offset = original->chunk * STR_CHUNK_BYTES;
    size_t previous = offset < original->length ? original->length - offset : 0;
    size_t current = offset < length ? length - offset : 0;
    if (previous > STR_CHUNK_BYTES)
        previous = STR_CHUNK_BYTES;
    if (current > STR_CHUNK_BYTES)
        current = STR_CHUNK_BYTES;
    memset(sector + PAYLOAD, 0, previous);
    if (current)
        memcpy(sector + PAYLOAD, frame + offset, current);
    write32(sector + STR_HEADER + 12, (uint32_t)length);
    memcpy(sector + STR_HEADER + 20, frame, 8);
    if (!(sector[SUBHEADER + 2] & 0x20) || read32(sector + FORM2_EDC))
        str_write_integrity(sector);
}
