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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>

#ifndef ACCESS_ONCE
#define ACCESS_ONCE(x) (*(volatile __typeof__((x)) *)&(x))
#endif
#ifndef READ_ONCE
#define READ_ONCE(x) ACCESS_ONCE(x)
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, v) (ACCESS_ONCE(x) = (v))
#endif

#if defined(__x86_64__) || defined(__i386__)
#define CPU_STORE_FENCE() asm volatile("sfence" ::: "memory")
#define CPU_LOAD_FENCE() asm volatile("lfence" ::: "memory")
#define CPU_FULL_FENCE() asm volatile("mfence" ::: "memory")
#elif defined(__aarch64__)
#define CPU_STORE_FENCE() asm volatile("dmb st" ::: "memory")
#define CPU_LOAD_FENCE() asm volatile("dmb sy" ::: "memory")
#define CPU_FULL_FENCE() asm volatile("dmb sy" ::: "memory")
#elif defined(__powerpc64__)
#define CPU_STORE_FENCE() asm volatile("sync" ::: "memory")
#define CPU_LOAD_FENCE() asm volatile("sync" ::: "memory")
#define CPU_FULL_FENCE() asm volatile("sync" ::: "memory")
#else
#define CPU_STORE_FENCE() asm volatile("" ::: "memory")
#define CPU_LOAD_FENCE() asm volatile("" ::: "memory")
#define CPU_FULL_FENCE() asm volatile("" ::: "memory")
#endif

#define MYCLOCK CLOCK_MONOTONIC
#define PAGE_ROUND_DOWN(x, n) ((x) & ~((n) - 1))
#define PAGE_ROUND_UP(x, n) (((x) + ((n) - 1)) & ~((n) - 1))

static size_t max_size = (size_t)1 << 23;
static size_t bar_offset = 0;
static int num_write_iters = 10000;
static int num_read_iters = 100;
static int warmup_iters = 32;
static int cpu_id = -1;
static bool do_read = true;
static bool do_write = false;
static bool subtract_empty_loop = true;
static bool use_avx_copy = false;
static bool use_sse41_copy = false;
static std::string resource_path;

#if defined(__x86_64__) || defined(__i386__)
extern "C" int memcpy_uncached_store_avx(void *dest, const void *src, size_t n_bytes);
extern "C" int memcpy_uncached_load_sse41(void *dest, const void *src, size_t n_bytes);

static bool cpu_has_avx()
{
#if defined(__GNUC__)
    return __builtin_cpu_supports("avx");
#else
    return false;
#endif
}

static bool cpu_has_sse41()
{
#if defined(__GNUC__)
    return __builtin_cpu_supports("sse4.1");
#else
    return false;
#endif
}
#else
static bool cpu_has_avx() { return false; }
static bool cpu_has_sse41() { return false; }
#endif

static void die_errno(const char *what)
{
    fprintf(stderr, "%s failed: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static void die_msg(const char *what)
{
    fprintf(stderr, "%s\n", what);
    exit(EXIT_FAILURE);
}

static double time_diff_us(const struct timespec &beg, const struct timespec &end)
{
    return ((end.tv_nsec - beg.tv_nsec) / 1000.0 + (end.tv_sec - beg.tv_sec) * 1000000.0);
}

static bool is_aligned(uintptr_t value, size_t pow2)
{
    return (value & (pow2 - 1)) == 0;
}

static void copy_to_bar(void *dst, const void *src, size_t size)
{
    void *curr_dst = dst;
    const void *curr_src = src;
    size_t remaining = size;

    if ((size == sizeof(uint8_t))) {
        WRITE_ONCE(*(uint8_t *)curr_dst, *(const uint8_t *)curr_src);
        CPU_STORE_FENCE();
        return;
    }
    if (size == sizeof(uint16_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint16_t))) {
        WRITE_ONCE(*(uint16_t *)curr_dst, *(const uint16_t *)curr_src);
        CPU_STORE_FENCE();
        return;
    }
    if (size == sizeof(uint32_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint32_t))) {
        WRITE_ONCE(*(uint32_t *)curr_dst, *(const uint32_t *)curr_src);
        CPU_STORE_FENCE();
        return;
    }
    if (size == sizeof(uint64_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint64_t))) {
        WRITE_ONCE(*(uint64_t *)curr_dst, *(const uint64_t *)curr_src);
        CPU_STORE_FENCE();
        return;
    }

#if defined(__x86_64__) || defined(__i386__)
    if (use_avx_copy) {
        memcpy_uncached_store_avx(dst, src, size);
        return;
    }
#endif

    while (remaining > 0) {
        size_t copied;
        if (remaining >= sizeof(uint64_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint64_t)) && is_aligned((uintptr_t)curr_src, sizeof(uint64_t))) {
            WRITE_ONCE(*(uint64_t *)curr_dst, *(const uint64_t *)curr_src);
            copied = sizeof(uint64_t);
        } else if (remaining >= sizeof(uint32_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint32_t)) && is_aligned((uintptr_t)curr_src, sizeof(uint32_t))) {
            WRITE_ONCE(*(uint32_t *)curr_dst, *(const uint32_t *)curr_src);
            copied = sizeof(uint32_t);
        } else if (remaining >= sizeof(uint16_t) && is_aligned((uintptr_t)curr_dst, sizeof(uint16_t)) && is_aligned((uintptr_t)curr_src, sizeof(uint16_t))) {
            WRITE_ONCE(*(uint16_t *)curr_dst, *(const uint16_t *)curr_src);
            copied = sizeof(uint16_t);
        } else {
            WRITE_ONCE(*(uint8_t *)curr_dst, *(const uint8_t *)curr_src);
            copied = sizeof(uint8_t);
        }
        curr_dst = (void *)((uintptr_t)curr_dst + copied);
        curr_src = (const void *)((uintptr_t)curr_src + copied);
        remaining -= copied;
    }
    CPU_STORE_FENCE();
}

static void copy_from_bar(void *dst, const void *src, size_t size)
{
    void *curr_dst = dst;
    const void *curr_src = src;
    size_t remaining = size;

#if defined(__x86_64__) || defined(__i386__)
    if (use_sse41_copy) {
        memcpy_uncached_load_sse41(dst, src, size);
        return;
    }
#endif

    while (remaining > 0) {
        size_t copied;
        if (remaining >= sizeof(uint64_t) && is_aligned((uintptr_t)curr_src, sizeof(uint64_t)) && is_aligned((uintptr_t)curr_dst, sizeof(uint64_t))) {
            *(uint64_t *)curr_dst = READ_ONCE(*(const uint64_t *)curr_src);
            copied = sizeof(uint64_t);
        } else if (remaining >= sizeof(uint32_t) && is_aligned((uintptr_t)curr_src, sizeof(uint32_t)) && is_aligned((uintptr_t)curr_dst, sizeof(uint32_t))) {
            *(uint32_t *)curr_dst = READ_ONCE(*(const uint32_t *)curr_src);
            copied = sizeof(uint32_t);
        } else if (remaining >= sizeof(uint16_t) && is_aligned((uintptr_t)curr_src, sizeof(uint16_t)) && is_aligned((uintptr_t)curr_dst, sizeof(uint16_t))) {
            *(uint16_t *)curr_dst = READ_ONCE(*(const uint16_t *)curr_src);
            copied = sizeof(uint16_t);
        } else {
            *(uint8_t *)curr_dst = READ_ONCE(*(const uint8_t *)curr_src);
            copied = sizeof(uint8_t);
        }
        curr_dst = (void *)((uintptr_t)curr_dst + copied);
        curr_src = (const void *)((uintptr_t)curr_src + copied);
        remaining -= copied;
    }
    CPU_LOAD_FENCE();
}

static void empty_loop(size_t size, int iters)
{
    volatile size_t sink = 0;
    for (int i = 0; i < iters; ++i)
        sink += size + (size_t)i;
}

static double measure_empty_loop(size_t size, int iters)
{
    struct timespec beg, end;
    clock_gettime(MYCLOCK, &beg);
    empty_loop(size, iters);
    clock_gettime(MYCLOCK, &end);
    return time_diff_us(beg, end);
}

static double measure_copy_to(void *bar_ptr, const void *host_buf, size_t size, int iters)
{
    struct timespec beg, end;
    for (int i = 0; i < warmup_iters; ++i)
        copy_to_bar(bar_ptr, host_buf, size);
    clock_gettime(MYCLOCK, &beg);
    for (int i = 0; i < iters; ++i)
        copy_to_bar(bar_ptr, host_buf, size);
    clock_gettime(MYCLOCK, &end);
    double elapsed = time_diff_us(beg, end);
    if (subtract_empty_loop)
        elapsed -= measure_empty_loop(size, iters);
    elapsed = std::max(0.0, elapsed);
    return elapsed / (double)iters;
}

static double measure_copy_from(void *host_buf, const void *bar_ptr, size_t size, int iters)
{
    struct timespec beg, end;
    for (int i = 0; i < warmup_iters; ++i)
        copy_from_bar(host_buf, bar_ptr, size);
    clock_gettime(MYCLOCK, &beg);
    for (int i = 0; i < iters; ++i)
        copy_from_bar(host_buf, bar_ptr, size);
    clock_gettime(MYCLOCK, &end);
    double elapsed = time_diff_us(beg, end);
    if (subtract_empty_loop)
        elapsed -= measure_empty_loop(size, iters);
    elapsed = std::max(0.0, elapsed);
    return elapsed / (double)iters;
}

static long parse_long(const char *value, const char *name)
{
    char *end = NULL;
    errno = 0;
    long result = strtol(value, &end, 0);
    if (errno || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return result;
}

static size_t parse_size(const char *value, const char *name, bool allow_zero = false)
{
    char *end = NULL;
    errno = 0;
    unsigned long long result = strtoull(value, &end, 0);
    if (errno || end == value) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(EXIT_FAILURE);
    }
    if (*end != '\0') {
        if ((end[1] != '\0') || (*end != 'k' && *end != 'K' && *end != 'm' && *end != 'M' && *end != 'g' && *end != 'G')) {
            fprintf(stderr, "invalid %s: %s\n", name, value);
            exit(EXIT_FAILURE);
        }
        if (*end == 'k' || *end == 'K')
            result <<= 10;
        else if (*end == 'm' || *end == 'M')
            result <<= 20;
        else
            result <<= 30;
    }
    if (!allow_zero && result == 0)
        die_msg("size must be non-zero");
    return (size_t)result;
}

static void print_usage(const char *path)
{
    std::cout << "Usage: " << path << " -f /sys/bus/pci/devices/<BDF>/resource<N> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "   -f <file>       PCI sysfs resource file to mmap (required)\n";
    std::cout << "   -s <size>       Maximum copy size, accepts K/M/G suffixes (default: " << max_size << ")\n";
    std::cout << "   -o <offset>     BAR offset to benchmark (default: " << bar_offset << ")\n";
    std::cout << "   -r <iters>      Number of read iterations per size (default: " << num_read_iters << ")\n";
    std::cout << "   -w <iters>      Number of write iterations per size (default: " << num_write_iters << ")\n";
    std::cout << "   -R              Benchmark CPU reads from BAR (default: yes)\n";
    std::cout << "   -W              Benchmark CPU writes to BAR (default: no)\n";
    std::cout << "   -B              Benchmark both reads and writes\n";
    std::cout << "   -p <cpu>        Pin this process to a CPU (default: no pinning)\n";
    std::cout << "   -n              Do not subtract empty-loop timing overhead\n";
    std::cout << "   -h              Print this help text\n\n";
    std::cout << "Example:\n";
    std::cout << "   " << path << " -f /sys/bus/pci/devices/0000:65:00.0/resource1 -s 8M -R\n";
    std::cout << "   sudo " << path << " -f /sys/bus/pci/devices/0000:65:00.0/resource1 -s 8M -B\n";
}

static void pin_cpu_if_requested()
{
    if (cpu_id < 0)
        return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        die_errno("sched_setaffinity");
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "f:s:o:r:w:p:RWBnh")) != -1) {
        switch (c) {
            case 'f':
                resource_path = optarg;
                break;
            case 's':
                max_size = parse_size(optarg, "size");
                break;
            case 'o':
                bar_offset = parse_size(optarg, "offset", true);
                break;
            case 'r':
                num_read_iters = (int)parse_long(optarg, "read iterations");
                break;
            case 'w':
                num_write_iters = (int)parse_long(optarg, "write iterations");
                break;
            case 'p':
                cpu_id = (int)parse_long(optarg, "cpu");
                break;
            case 'R':
                do_read = true;
                do_write = false;
                break;
            case 'W':
                do_read = false;
                do_write = true;
                break;
            case 'B':
                do_read = true;
                do_write = true;
                break;
            case 'n':
                subtract_empty_loop = false;
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (resource_path.empty()) {
        print_usage(argv[0]);
        die_msg("ERROR: -f <resource file> is required");
    }
    if (num_read_iters <= 0 || num_write_iters <= 0)
        die_msg("iteration counts must be positive");

    pin_cpu_if_requested();
    use_avx_copy = cpu_has_avx();
    use_sse41_copy = cpu_has_sse41();

    long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0)
        die_errno("sysconf(_SC_PAGESIZE)");
    const size_t page_size = (size_t)page_size_long;
    const off_t map_offset = (off_t)PAGE_ROUND_DOWN(bar_offset, page_size);
    const size_t page_delta = bar_offset - (size_t)map_offset;
    const size_t map_size = PAGE_ROUND_UP(page_delta + max_size, page_size);

    int open_flags = do_write ? O_RDWR : O_RDONLY;
#ifdef O_SYNC
    open_flags |= O_SYNC;
#endif
    int fd = open(resource_path.c_str(), open_flags);
    if (fd < 0)
        die_errno("open");

    int prot = PROT_READ | (do_write ? PROT_WRITE : 0);
    void *map_base = mmap(NULL, map_size, prot, MAP_SHARED, fd, map_offset);
    if (map_base == MAP_FAILED)
        die_errno("mmap");

    void *bar_ptr = (void *)((uintptr_t)map_base + page_delta);

    void *host_buf = NULL;
    if (posix_memalign(&host_buf, 64, max_size) != 0)
        die_msg("posix_memalign failed");
    memset(host_buf, 0xa5, max_size);
    (void)mlock(host_buf, max_size);

    std::cout << "resource file: " << resource_path << std::endl;
    std::cout << "BAR offset: 0x" << std::hex << bar_offset << std::dec << std::endl;
    std::cout << "mapped size: " << map_size << std::endl;
    std::cout << "user-space BAR pointer: " << bar_ptr << std::endl;
    std::cout << "subtract empty loop: " << (subtract_empty_loop ? "yes" : "no") << std::endl;
    std::cout << "optimized copy: "
              << "avx_write=" << (use_avx_copy ? "yes" : "no")
              << ", sse4.1_read=" << (use_sse41_copy ? "yes" : "no") << std::endl;
    if (cpu_id >= 0)
        std::cout << "pinned CPU: " << cpu_id << std::endl;

    CPU_FULL_FENCE();

    if (do_write) {
        std::cout << std::endl;
        std::cout << "pcibar_copy_to_mapping num iters for each size: " << num_write_iters << std::endl;
        std::cout << "WARNING: writes to a PCI BAR can modify device memory/registers; pass only a safe GPU memory BAR/window." << std::endl;
        printf("Test \t\t\t Size(B) \t Avg.Time(us)\n");
        for (size_t copy_size = 1; copy_size <= max_size; copy_size <<= 1) {
            double lat_us = measure_copy_to(bar_ptr, host_buf, copy_size, num_write_iters);
            printf("pcibar_copy_to_mapping \t %8zu \t %11.4f\n", copy_size, lat_us);
        }
    }

    if (do_read) {
        std::cout << std::endl;
        std::cout << "pcibar_copy_from_mapping num iters for each size: " << num_read_iters << std::endl;
        printf("Test \t\t\t Size(B) \t Avg.Time(us)\n");
        for (size_t copy_size = 1; copy_size <= max_size; copy_size <<= 1) {
            double lat_us = measure_copy_from(host_buf, bar_ptr, copy_size, num_read_iters);
            printf("pcibar_copy_from_mapping \t %8zu \t %11.4f\n", copy_size, lat_us);
        }
    }

    munlock(host_buf, max_size);
    free(host_buf);
    if (munmap(map_base, map_size) != 0)
        die_errno("munmap");
    close(fd);

    return EXIT_SUCCESS;
}
