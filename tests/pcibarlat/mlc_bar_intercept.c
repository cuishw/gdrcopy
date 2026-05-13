/*
 * Copyright (c) 2026. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define DEFAULT_MIN_ALLOC ((size_t)1 << 20)

static void *(*real_mmap_fn)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64_fn)(void *, size_t, int, int, int, off64_t);
static int (*real_munmap_fn)(void *, size_t);
static void *(*real_malloc_fn)(size_t);
static void (*real_free_fn)(void *);
static int (*real_posix_memalign_fn)(void **, size_t, size_t);
static void *(*real_aligned_alloc_fn)(size_t, size_t);
static void *(*real_memalign_fn)(size_t, size_t);
static void *(*real_valloc_fn)(size_t);
static void *(*real_pvalloc_fn)(size_t);

static pthread_mutex_t bar_lock = PTHREAD_MUTEX_INITIALIZER;
static int init_done;
static int init_running;
static int verbose;
static int intercept_mmap = 1;
static int intercept_alloc;
static int bar_fd = -1;
static void *bar_base;
static size_t bar_size;
static size_t bar_offset;
static size_t bar_next;
static size_t page_size;
static size_t min_alloc = DEFAULT_MIN_ALLOC;
static const char *resource_path;

static size_t round_up_pow2(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static size_t parse_size_env(const char *value, size_t default_value)
{
    char *end;
    unsigned long long result;

    if (!value || !*value)
        return default_value;

    errno = 0;
    result = strtoull(value, &end, 0);
    if (errno || end == value)
        return default_value;

    if (*end == 'k' || *end == 'K')
        result <<= 10;
    else if (*end == 'm' || *end == 'M')
        result <<= 20;
    else if (*end == 'g' || *end == 'G')
        result <<= 30;
    else if (*end != '\0')
        return default_value;

    return (size_t)result;
}

static void resolve_real_symbols(void)
{
    real_mmap_fn = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    real_mmap64_fn = (void *(*)(void *, size_t, int, int, int, off64_t))dlsym(RTLD_NEXT, "mmap64");
    real_munmap_fn = (int (*)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
    real_malloc_fn = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_free_fn = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    real_posix_memalign_fn = (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc_fn = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign_fn = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
    real_valloc_fn = (void *(*)(size_t))dlsym(RTLD_NEXT, "valloc");
    real_pvalloc_fn = (void *(*)(size_t))dlsym(RTLD_NEXT, "pvalloc");
}

static void init_once(void)
{
    const char *mode;
    void *mapped;

    if (init_done || init_running)
        return;

    init_running = 1;
    resolve_real_symbols();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (!page_size)
        page_size = 4096;

    verbose = getenv("PCIBAR_MLC_VERBOSE") != NULL;
    resource_path = getenv("PCIBAR_MLC_RESOURCE");
    min_alloc = parse_size_env(getenv("PCIBAR_MLC_MIN_ALLOC"), DEFAULT_MIN_ALLOC);
    bar_size = parse_size_env(getenv("PCIBAR_MLC_SIZE"), 0);
    bar_offset = parse_size_env(getenv("PCIBAR_MLC_OFFSET"), 0);

    mode = getenv("PCIBAR_MLC_INTERCEPT");
    if (mode) {
        intercept_mmap = !strcmp(mode, "mmap") || !strcmp(mode, "all");
        intercept_alloc = !strcmp(mode, "alloc") || !strcmp(mode, "all");
    }

    if (!resource_path || !*resource_path || bar_size == 0 || !real_mmap_fn) {
        if (verbose)
            fprintf(stderr, "pcibar_mlc_intercept: disabled; set PCIBAR_MLC_RESOURCE and PCIBAR_MLC_SIZE\n");
        init_done = 1;
        init_running = 0;
        return;
    }

    bar_fd = open(resource_path, O_RDWR | O_SYNC);
    if (bar_fd < 0)
        bar_fd = open(resource_path, O_RDONLY | O_SYNC);
    if (bar_fd < 0) {
        fprintf(stderr, "pcibar_mlc_intercept: open(%s) failed: %s\n", resource_path, strerror(errno));
        init_done = 1;
        init_running = 0;
        return;
    }

    mapped = real_mmap_fn(NULL, bar_size, PROT_READ | PROT_WRITE, MAP_SHARED, bar_fd, (off_t)bar_offset);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "pcibar_mlc_intercept: mmap(%s, size=%zu, offset=%zu) failed: %s\n",
                resource_path, bar_size, bar_offset, strerror(errno));
        close(bar_fd);
        bar_fd = -1;
        init_done = 1;
        init_running = 0;
        return;
    }

    bar_base = mapped;
    if (verbose) {
        fprintf(stderr,
                "pcibar_mlc_intercept: mapped %s offset=0x%zx size=%zu at %p; mode=%s%s min_alloc=%zu\n",
                resource_path, bar_offset, bar_size, bar_base,
                intercept_mmap ? "mmap" : "", intercept_alloc ? "+alloc" : "", min_alloc);
    }

    init_done = 1;
    init_running = 0;
}

static int is_bar_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)bar_base;
    uintptr_t end = start + bar_size;

    return bar_base && value >= start && value < end;
}

static void *bar_alloc(size_t size, size_t align)
{
    size_t start;
    size_t end;
    void *ptr = NULL;

    init_once();
    if (!bar_base || size < min_alloc)
        return NULL;

    if (align < page_size)
        align = page_size;
    if ((align & (align - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&bar_lock);
    start = round_up_pow2(bar_next, align);
    end = start + size;
    if (end >= start && end <= bar_size) {
        ptr = (char *)bar_base + start;
        bar_next = round_up_pow2(end, page_size);
    }
    pthread_mutex_unlock(&bar_lock);

    if (verbose && ptr) {
        fprintf(stderr, "pcibar_mlc_intercept: allocation size=%zu align=%zu -> %p (bar+0x%zx)\n",
                size, align, ptr, start);
    }

    return ptr;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    int anonymous = (flags & MAP_ANONYMOUS) || fd == -1;
    void *ptr;

    init_once();
    if (!init_running && intercept_mmap && anonymous && !(flags & MAP_FIXED) && length >= min_alloc) {
        ptr = bar_alloc(length, page_size);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
    }

    return real_mmap_fn(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
    int anonymous = (flags & MAP_ANONYMOUS) || fd == -1;
    void *ptr;

    init_once();
    if (!init_running && intercept_mmap && anonymous && !(flags & MAP_FIXED) && length >= min_alloc) {
        ptr = bar_alloc(length, page_size);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
    }

    if (real_mmap64_fn)
        return real_mmap64_fn(addr, length, prot, flags, fd, offset);
    return real_mmap_fn(addr, length, prot, flags, fd, (off_t)offset);
}

int munmap(void *addr, size_t length)
{
    (void)length;
    init_once();
    if (is_bar_pointer(addr)) {
        if (verbose)
            fprintf(stderr, "pcibar_mlc_intercept: suppress munmap(%p) for BAR-backed allocation\n", addr);
        return 0;
    }
    return real_munmap_fn(addr, length);
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *ptr;

    init_once();
    if (intercept_alloc) {
        ptr = bar_alloc(size, alignment);
        if (ptr) {
            *memptr = ptr;
            return 0;
        }
        if (bar_base)
            return ENOMEM;
    }
    return real_posix_memalign_fn(memptr, alignment, size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    void *ptr;

    init_once();
    if (intercept_alloc) {
        ptr = bar_alloc(size, alignment);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return real_aligned_alloc_fn(alignment, size);
}

void *memalign(size_t alignment, size_t size)
{
    void *ptr;

    init_once();
    if (intercept_alloc) {
        ptr = bar_alloc(size, alignment);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return real_memalign_fn(alignment, size);
}

void *valloc(size_t size)
{
    void *ptr;

    init_once();
    if (intercept_alloc) {
        ptr = bar_alloc(size, page_size);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return real_valloc_fn(size);
}

void *pvalloc(size_t size)
{
    void *ptr;

    init_once();
    if (intercept_alloc) {
        ptr = bar_alloc(round_up_pow2(size, page_size), page_size);
        if (ptr)
            return ptr;
        if (bar_base) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return real_pvalloc_fn(size);
}

void *malloc(size_t size)
{
    void *ptr;

    init_once();
    if (!init_running && intercept_alloc) {
        ptr = bar_alloc(size, page_size);
        if (ptr)
            return ptr;
        if (bar_base && size >= min_alloc) {
            errno = ENOMEM;
            return NULL;
        }
    }
    if (!real_malloc_fn) {
        errno = ENOMEM;
        return NULL;
    }
    return real_malloc_fn(size);
}

void free(void *ptr)
{
    init_once();
    if (is_bar_pointer(ptr)) {
        if (verbose)
            fprintf(stderr, "pcibar_mlc_intercept: suppress free(%p) for BAR-backed allocation\n", ptr);
        return;
    }
    if (real_free_fn)
        real_free_fn(ptr);
}

__attribute__((destructor)) static void fini_bar_mapping(void)
{
    if (bar_base && real_munmap_fn)
        real_munmap_fn(bar_base, bar_size);
    if (bar_fd >= 0)
        close(bar_fd);
}
