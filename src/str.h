#ifndef PSXRC_STR_H
#define PSXRC_STR_H

#include <stddef.h>
#include <stdint.h>

#define STR_SECTOR_BYTES 2352
#define STR_CHUNK_BYTES 2016
#define STR_MAX_CHUNKS 32
#define STR_MAX_FRAME_BYTES (STR_CHUNK_BYTES * STR_MAX_CHUNKS)

typedef struct {
    unsigned chunk, chunks, width, height, version, words, quant;
    uint32_t number, length;
    uint8_t file, channel;
} str_header;

/* Strict standard Mode 2 STR video sector with valid integrity fields. */
int str_parse(const uint8_t sector[STR_SECTOR_BYTES], str_header *header);
int str_same_frame(const str_header *a, const str_header *b);
uint32_t str_edc(const uint8_t *data, size_t size);
void str_write_integrity(uint8_t sector[STR_SECTOR_BYTES]);
int str_form1_integrity(const uint8_t sector[STR_SECTOR_BYTES]);
int str_has_signature(const uint8_t sector[STR_SECTOR_BYTES]);
void str_make_template(uint8_t sector[STR_SECTOR_BYTES], const str_header *original);
void str_replace(uint8_t sector[STR_SECTOR_BYTES], const str_header *original,
                 const uint8_t *frame, size_t length);

#endif
