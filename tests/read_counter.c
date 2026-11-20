/* Intercepts read() so tests can assert how many file bytes a loadSection()
 * call actually pulls from the storage layer. Built as a shared library and
 * preloaded via LD_PRELOAD; see RangeLoadingTests.cpp. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

static _Thread_local int g_counting = 0;
static _Thread_local uint64_t g_readBytes = 0;

ssize_t read (int fd, void* buffer, size_t count)
{
    static ssize_t (*real_read) (int, void*, size_t) = NULL;

    if (real_read == NULL)
        real_read = (ssize_t (*) (int, void*, size_t)) dlsym (RTLD_NEXT, "read");

    ssize_t result = real_read (fd, buffer, count);

    if (g_counting && result > 0)
        g_readBytes += (uint64_t) result;

    return result;
}

void audiofile_test_set_counting (int enabled)
{
    g_counting = enabled;

    if (enabled)
        g_readBytes = 0;
}

uint64_t audiofile_test_get_read_bytes (void)
{
    return g_readBytes;
}
