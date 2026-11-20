#ifndef PSXRC_FORMAT_H
#define PSXRC_FORMAT_H

#include "psxrc.h"

#define PSXRC_MAGIC "PSXRC\r\n\032"
#define PSXRC_VERSION 1
#define PSXRC_HUNK_SECTORS 8
#define PSXRC_MAX_SECTORS 449850u
#define PSXRC_HEADER_BYTES 48
#define PSXRC_TRACK_BYTES 16
#define PSXRC_RECORD_BYTES 16
#define PSXRC_MAP_BYTES 8
#define PSXRC_FRAME_BYTES 20
#define PSXRC_SUB_RECORD_BYTES 97
#define PSXRC_FLAG_SUBCHANNEL 1
#define PSXRC_FLAG_SBI 2
#define PSXRC_FLAG_FILTERS 4
#define PSXRC_FILTER_BYTES 2
#define PSXRC_NO_INDEX UINT32_MAX

typedef struct {
    uint64_t offset;
    uint32_t bytes, checksum;
} psxrc_record;

#endif
