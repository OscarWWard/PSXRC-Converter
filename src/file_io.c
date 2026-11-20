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

#ifdef _WIN32
static wchar_t *file_wide(const char *text)
{
    unsigned codepage = CP_UTF8;
    int count = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) {
        codepage = CP_ACP;
        count = MultiByteToWideChar(codepage, 0, text, -1, NULL, 0);
    }
    wchar_t *wide = count ? malloc((size_t)count * sizeof(*wide)) : NULL;
    if (wide && !MultiByteToWideChar(codepage, 0, text, -1, wide, count)) {
        free(wide);
        return NULL;
    }
    return wide;
}

char *file_utf8(const wchar_t *text)
{
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *out = count ? malloc((size_t)count) : NULL;
    if (out && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, count, NULL, NULL)) {
        free(out);
        return NULL;
    }
    return out;
}

char *file_normalize_utf8(const char *text)
{
    wchar_t *wide = file_wide(text);
    char *utf8 = wide ? file_utf8(wide) : NULL;
    free(wide);
    return utf8;
}
#endif

FILE *file_open(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide_path = file_wide(path), *wide_mode = file_wide(mode);
    FILE *file = wide_path && wide_mode ? _wfopen(wide_path, wide_mode) : NULL;
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

int file_remove(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = file_wide(path);
    int result = wide ? _wremove(wide) : -1;
    free(wide);
    return result;
#else
    return remove(path);
#endif
}

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
        FILE *file = file_open(name, "w+bx");
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
    wchar_t *source = file_wide(temp_path), *destination = file_wide(path);
    int moved = source && destination && MoveFileExW(source, destination, MOVEFILE_WRITE_THROUGH);
    free(source);
    free(destination);
    return moved;
#else
    if (link(temp_path, path) != 0)
        return 0;
    unlink(temp_path);
    return 1;
#endif
}
