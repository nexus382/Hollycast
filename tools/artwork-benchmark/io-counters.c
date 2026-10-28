#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct artwork_io_counts {
    uint64_t read_bytes, write_bytes, read_calls, write_calls, cue_opens, bin_opens;
};

static _Atomic int active;
static _Atomic uint64_t read_bytes, write_bytes, read_calls, write_calls, cue_opens, bin_opens;

void artwork_io_reset(void)
{
    atomic_store(&active, 0);
    atomic_store(&read_bytes, 0);
    atomic_store(&write_bytes, 0);
    atomic_store(&read_calls, 0);
    atomic_store(&write_calls, 0);
    atomic_store(&cue_opens, 0);
    atomic_store(&bin_opens, 0);
    atomic_store(&active, 1);
}

void artwork_io_stop(struct artwork_io_counts *counts)
{
    atomic_store(&active, 0);
    *counts = (struct artwork_io_counts){
        atomic_load(&read_bytes), atomic_load(&write_bytes),
        atomic_load(&read_calls), atomic_load(&write_calls),
        atomic_load(&cue_opens), atomic_load(&bin_opens)
    };
}

static size_t measured_fread(void *buffer, size_t size, size_t count, FILE *file)
{
    const size_t result = fread(buffer, size, count, file);
    if (atomic_load(&active)) {
        atomic_fetch_add(&read_bytes, result * size);
        atomic_fetch_add(&read_calls, 1);
    }
    return result;
}

static char *measured_fgets(char *buffer, int size, FILE *file)
{
    char *result = fgets(buffer, size, file);
    if (result != NULL && atomic_load(&active)) {
        atomic_fetch_add(&read_bytes, strlen(result));
        atomic_fetch_add(&read_calls, 1);
    }
    return result;
}

static size_t measured_fwrite(const void *buffer, size_t size, size_t count, FILE *file)
{
    const size_t result = fwrite(buffer, size, count, file);
    if (atomic_load(&active)) {
        atomic_fetch_add(&write_bytes, result * size);
        atomic_fetch_add(&write_calls, 1);
    }
    return result;
}

static FILE *measured_fopen(const char *path, const char *mode)
{
    FILE *result = fopen(path, mode);
    if (result != NULL && atomic_load(&active)) {
        const size_t length = strlen(path);
        if (length >= 4 && strcmp(path + length - 4, ".cue") == 0)
            atomic_fetch_add(&cue_opens, 1);
        if (length >= 4 && strcmp(path + length - 4, ".bin") == 0)
            atomic_fetch_add(&bin_opens, 1);
    }
    return result;
}

#define INTERPOSE(replacement, original) \
    __attribute__((used, section("__DATA,__interpose"))) \
    static struct { const void *replacement; const void *original; } \
    interpose_##original = { (const void *)&replacement, (const void *)&original }

INTERPOSE(measured_fread, fread);
INTERPOSE(measured_fgets, fgets);
INTERPOSE(measured_fwrite, fwrite);
INTERPOSE(measured_fopen, fopen);
