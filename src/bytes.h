#ifndef PSXRC_BYTES_H
#define PSXRC_BYTES_H

#include <stdint.h>

static inline uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (unsigned)p[1] << 8);
}

static inline uint32_t read32(const uint8_t *p)
{
    return read16(p) | (uint32_t)read16(p + 2) << 16;
}

static inline uint64_t read64(const uint8_t *p)
{
    return read32(p) | (uint64_t)read32(p + 4) << 32;
}

static inline void write16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void write32(uint8_t *p, uint32_t value)
{
    write16(p, value);
    write16(p + 2, value >> 16);
}

static inline void write64(uint8_t *p, uint64_t value)
{
    write32(p, (uint32_t)value);
    write32(p + 4, (uint32_t)(value >> 32));
}

#endif
