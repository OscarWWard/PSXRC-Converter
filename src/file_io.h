#ifndef PSXRC_FILE_IO_H
#define PSXRC_FILE_IO_H

#include <stdio.h>
#include <stdint.h>

FILE *file_open(const char *path, const char *mode);
int file_remove(const char *path);
#ifdef _WIN32
char *file_utf8(const wchar_t *text);
char *file_normalize_utf8(const char *text);
#endif
int file_size(FILE *file, uint64_t *size);
int file_seek(FILE *file, uint64_t offset);
int file_read_at(void *ctx, uint64_t offset, void *out, size_t size);
FILE *file_create_temp(const char *path, char **temp_path);
int file_publish(const char *temp_path, const char *path);

#endif
