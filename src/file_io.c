#include "file_io.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define seek_impl _fseeki64
#define tell_impl _ftelli64
#else
#include <unistd.h>
#define seek_impl fseeko
#define tell_impl ftello
#endif

int file_size(FILE *file, uint64_t *size)
{
    if (seek_impl(file, 0, SEEK_END) != 0)
        return 0;
    int64_t position = tell_impl(file);
    if (position < 0)
        return 0;
    *size = (uint64_t)position;
    return 1;
}

int file_seek(FILE *file, uint64_t offset)
{
    return offset <= INT64_MAX && seek_impl(file, (int64_t)offset, SEEK_SET) == 0;
}

int file_read_at(void *ctx, uint64_t offset, void *out, size_t size)
{
    FILE *file = ctx;
    return file_seek(file, offset) && fread(out, 1, size, file) == size;
}

FILE *file_create_temp(const char *path, char **temp_path)
{
    size_t size = strlen(path) + 32;
    char *name = malloc(size);
    if (!name)
        return NULL;
    for (unsigned i = 0; i < 100; ++i) {
#ifdef _WIN32
        unsigned long pid = GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        snprintf(name, size, "%s.%lu.%u.tmp", path, pid, i);
        FILE *file = fopen(name, "w+bx");
        if (file) {
            *temp_path = name;
            return file;
        }
    }
    free(name);
    return NULL;
}

int file_publish(const char *temp_path, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(temp_path, path, MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (link(temp_path, path) != 0)
        return 0;
    unlink(temp_path);
    return 1;
#endif
}
