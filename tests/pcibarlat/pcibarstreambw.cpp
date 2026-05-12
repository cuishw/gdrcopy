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
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

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

static size_t map_size_opt = (size_t)1 << 23;
static size_t bar_offset = 0;
static int num_iters = 100;
static int num_threads = 0;
static int first_cpu = -1;
static bool do_read = true;
static bool do_write = false;
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

struct worker_args {
    int index;
    int cpu_id;
    int iters;
    bool do_write;
    void *bar_ptr;
    void *host_buf;
    size_t size;
    pthread_barrier_t *start_barrier;
    pthread_barrier_t *end_barrier;
};

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

static int get_default_threads()
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus <= 0)
        return 1;
    return (int)cpus;
}

static void print_usage(const char *path)
{
    std::cout << "Usage: " << path << " -f /sys/bus/pci/devices/<BDF>/resource<N> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "   -f <file>       PCI sysfs resource file or pcibarlat_physmem device to mmap (required)\n";
    std::cout << "   -s <size>       Mapping size, accepts K/M/G suffixes (default: " << map_size_opt << ")\n";
    std::cout << "   -i <iters>      Number of full-range stream iterations (default: " << num_iters << ")\n";
    std::cout << "   -t <threads>    Number of worker threads (default: online CPUs)\n";
    std::cout << "   -o <offset>     BAR offset to benchmark (default: " << bar_offset << ")\n";
    std::cout << "   -p <cpu>        Pin worker 0 to this CPU and subsequent workers to following CPUs\n";
    std::cout << "   -R              Benchmark CPU reads from BAR (default: yes)\n";
    std::cout << "   -W              Benchmark CPU writes to BAR (default: no)\n";
    std::cout << "   -B              Benchmark both reads and writes\n";
    std::cout << "   -h              Print this help text\n\n";
    std::cout << "Example:\n";
    std::cout << "   " << path << " -f /sys/bus/pci/devices/0000:65:00.0/resource1_wc -s 256M -t 32 -R\n";
    std::cout << "   sudo " << path << " -f /dev/pcibarlat_physmem -s 256M -t 32 -B\n";
}

static void pin_worker_if_requested(int cpu_id)
{
    if (cpu_id < 0)
        return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        die_errno("sched_setaffinity");
}

static void *stream_worker(void *opaque)
{
    struct worker_args *args = (struct worker_args *)opaque;
    pin_worker_if_requested(args->cpu_id);

    memset(args->host_buf, 0xa5 ^ args->index, args->size);
    if (args->do_write) {
        copy_to_bar(args->bar_ptr, args->host_buf, args->size);
    } else {
        copy_from_bar(args->host_buf, args->bar_ptr, args->size);
    }

    pthread_barrier_wait(args->start_barrier);
    for (int iter = 0; iter < args->iters; ++iter) {
        if (args->do_write)
            copy_to_bar(args->bar_ptr, args->host_buf, args->size);
        else
            copy_from_bar(args->host_buf, args->bar_ptr, args->size);
    }
    pthread_barrier_wait(args->end_barrier);
    return NULL;
}

static double run_stream_test(void *bar_ptr, size_t size, bool write_test)
{
    pthread_barrier_t start_barrier;
    pthread_barrier_t end_barrier;
    std::vector<pthread_t> threads(num_threads);
    std::vector<struct worker_args> args(num_threads);
    std::vector<void *> host_bufs(num_threads, NULL);
    size_t worker_size = size / (size_t)num_threads;
    if (worker_size == 0)
        die_msg("mapping size is too small for the requested thread count");

    worker_size = (worker_size / 64) * 64;
    if (worker_size == 0)
        die_msg("per-thread mapping slice is too small after alignment");

    if (pthread_barrier_init(&start_barrier, NULL, num_threads + 1) != 0)
        die_msg("pthread_barrier_init failed");
    if (pthread_barrier_init(&end_barrier, NULL, num_threads + 1) != 0)
        die_msg("pthread_barrier_init failed");

    for (int i = 0; i < num_threads; ++i) {
        if (posix_memalign(&host_bufs[i], 64, worker_size) != 0)
            die_msg("posix_memalign failed");
        (void)mlock(host_bufs[i], worker_size);

        args[i].index = i;
        args[i].cpu_id = first_cpu >= 0 ? first_cpu + i : -1;
        args[i].iters = num_iters;
        args[i].do_write = write_test;
        args[i].bar_ptr = (void *)((uintptr_t)bar_ptr + (uintptr_t)i * worker_size);
        args[i].host_buf = host_bufs[i];
        args[i].size = worker_size;
        args[i].start_barrier = &start_barrier;
        args[i].end_barrier = &end_barrier;
        if (pthread_create(&threads[i], NULL, stream_worker, &args[i]) != 0)
            die_errno("pthread_create");
    }

    struct timespec beg, end;
    clock_gettime(MYCLOCK, &beg);
    pthread_barrier_wait(&start_barrier);
    pthread_barrier_wait(&end_barrier);
    clock_gettime(MYCLOCK, &end);

    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
        munlock(host_bufs[i], worker_size);
        free(host_bufs[i]);
    }
    pthread_barrier_destroy(&start_barrier);
    pthread_barrier_destroy(&end_barrier);

    double elapsed_us = time_diff_us(beg, end);
    if (elapsed_us <= 0.0)
        return 0.0;
    double byte_count = (double)worker_size * (double)num_threads * (double)num_iters;
    return (byte_count / elapsed_us * 1e6) / 1024.0 / 1024.0;
}

int main(int argc, char **argv)
{
    num_threads = get_default_threads();

    int c;
    while ((c = getopt(argc, argv, "f:s:i:t:o:p:RWBh")) != -1) {
        switch (c) {
            case 'f':
                resource_path = optarg;
                break;
            case 's':
                map_size_opt = parse_size(optarg, "mapping size");
                break;
            case 'i':
                num_iters = (int)parse_long(optarg, "iterations");
                break;
            case 't':
                num_threads = (int)parse_long(optarg, "threads");
                break;
            case 'o':
                bar_offset = parse_size(optarg, "offset", true);
                break;
            case 'p':
                first_cpu = (int)parse_long(optarg, "cpu");
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
    if (num_iters <= 0 || num_threads <= 0)
        die_msg("iterations and thread count must be positive");

    use_avx_copy = cpu_has_avx();
    use_sse41_copy = cpu_has_sse41();

    long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0)
        die_errno("sysconf(_SC_PAGESIZE)");
    const size_t page_size = (size_t)page_size_long;
    const off_t map_offset = (off_t)PAGE_ROUND_DOWN(bar_offset, page_size);
    const size_t page_delta = bar_offset - (size_t)map_offset;
    const size_t map_size = PAGE_ROUND_UP(page_delta + map_size_opt, page_size);

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

    std::cout << "resource file: " << resource_path << std::endl;
    std::cout << "BAR offset: 0x" << std::hex << bar_offset << std::dec << std::endl;
    std::cout << "mapped size: " << map_size << std::endl;
    std::cout << "stream size: " << map_size_opt << std::endl;
    std::cout << "threads: " << num_threads << std::endl;
    std::cout << "iterations: " << num_iters << std::endl;
    std::cout << "user-space BAR pointer: " << bar_ptr << std::endl;
    std::cout << "optimized copy: "
              << "avx_write=" << (use_avx_copy ? "yes" : "no")
              << ", sse4.1_read=" << (use_sse41_copy ? "yes" : "no") << std::endl;
    if (first_cpu >= 0)
        std::cout << "pinned CPU range: " << first_cpu << "-" << (first_cpu + num_threads - 1) << std::endl;

    CPU_FULL_FENCE();

    printf("Test \t\t Threads \t Size(B) \t Avg.BW(MB/s)\n");
    if (do_write) {
        std::cout << "WARNING: writes to a PCI BAR can modify device memory/registers; pass only a safe GPU memory BAR/window." << std::endl;
        double bw = run_stream_test(bar_ptr, map_size_opt, true);
        printf("pcibar_stream_write_bw \t %7d \t %8zu \t %12.2f\n", num_threads, map_size_opt, bw);
    }
    if (do_read) {
        double bw = run_stream_test(bar_ptr, map_size_opt, false);
        printf("pcibar_stream_read_bw \t %7d \t %8zu \t %12.2f\n", num_threads, map_size_opt, bw);
    }

    if (munmap(map_base, map_size) != 0)
        die_errno("munmap");
    close(fd);

    return EXIT_SUCCESS;
}
