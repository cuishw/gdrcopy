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
#include <linux/idxd.h>
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

#include <iostream>
#include <string>
#include <vector>

#define MYCLOCK CLOCK_MONOTONIC
#define PAGE_ROUND_DOWN(x, n) ((x) & ~((n) - 1))
#define PAGE_ROUND_UP(x, n) (((x) + ((n) - 1)) & ~((n) - 1))

#ifndef DSA_COMP_STATUS_MASK
#define DSA_COMP_STATUS_MASK 0x7f
#endif
#ifndef DSA_COMP_STATUS
#define DSA_COMP_STATUS(status) ((status) & DSA_COMP_STATUS_MASK)
#endif

static size_t per_thread_size = (size_t)8 << 20;
static size_t bar_offset = 0;
static int num_iters = 100;
static int num_threads = 0;
static int first_cpu = -1;
static bool do_read = true;
static bool do_write = false;
static std::string resource_path;
static std::string dsa_wq_path = "/dev/dsa/wq0.0";

struct dsa_completion_record_aligned {
    struct dsa_completion_record record;
} __attribute__((aligned(32)));

struct worker_args {
    int index;
    int cpu_id;
    int iters;
    bool do_write;
    const char *dsa_wq_path;
    void *bar_ptr;
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

static int get_online_cpus()
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus <= 0)
        return 1;
    return (int)cpus;
}

static void print_usage(const char *path)
{
    std::cout << "Usage: " << path << " -f /dev/reserved_mem -q /dev/dsa/wq0.0 [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "   -f <file>       Memory/BAR file to mmap (required)\n";
    std::cout << "   -q <wq>         Intel DSA work-queue character device (default: " << dsa_wq_path << ")\n";
    std::cout << "   -s <size>       Per-thread DSA copy size, accepts K/M/G suffixes (default: " << per_thread_size << ")\n";
    std::cout << "   -i <iters>      Number of full-range DSA iterations (default: " << num_iters << ")\n";
    std::cout << "   -t <threads>    Number of worker threads (default: online CPUs)\n";
    std::cout << "   -o <offset>     Mapping offset to benchmark (default: " << bar_offset << ")\n";
    std::cout << "   -p <cpu>        Pin worker 0 to this CPU and subsequent workers to following CPUs\n";
    std::cout << "   -R              DSA copy from mapped memory to host buffers (default: yes)\n";
    std::cout << "   -W              DSA copy from host buffers to mapped memory (default: no)\n";
    std::cout << "   -B              Benchmark both directions\n";
    std::cout << "   -h              Print this help text\n\n";
    std::cout << "Example:\n";
    std::cout << "   sudo " << path << " -f /dev/reserved_mem -q /dev/dsa/wq0.0 -s 100M -t 56 -R\n";
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

static void submit_dsa_memmove(int wq_fd, void *dst, const void *src, size_t size,
                               struct dsa_completion_record_aligned *completion)
{
    struct dsa_hw_desc desc;
    memset(&desc, 0, sizeof(desc));
    memset(completion, 0, sizeof(*completion));

    desc.opcode = DSA_OPCODE_MEMMOVE;
    desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
    desc.completion_addr = (uint64_t)(uintptr_t)&completion->record;
    desc.src_addr = (uint64_t)(uintptr_t)src;
    desc.dst_addr = (uint64_t)(uintptr_t)dst;
    desc.xfer_size = (uint32_t)size;

    ssize_t rc = write(wq_fd, &desc, sizeof(desc));
    if (rc != (ssize_t)sizeof(desc))
        die_errno("write DSA descriptor");

    while (completion->record.status == DSA_COMP_NONE)
        asm volatile("pause" ::: "memory");

    uint8_t status = DSA_COMP_STATUS(completion->record.status);
    if (status != DSA_COMP_SUCCESS) {
        fprintf(stderr, "DSA completion failed: status=0x%x bytes_completed=%u fault_addr=0x%lx\n",
                status, completion->record.bytes_completed, (unsigned long)completion->record.fault_addr);
        exit(EXIT_FAILURE);
    }
}

static void *dsa_worker(void *opaque)
{
    struct worker_args *args = (struct worker_args *)opaque;
    pin_worker_if_requested(args->cpu_id);

    int wq_fd = open(args->dsa_wq_path, O_RDWR);
    if (wq_fd < 0)
        die_errno("open DSA work queue");

    void *host_buf = NULL;
    if (posix_memalign(&host_buf, 4096, args->size) != 0)
        die_msg("posix_memalign failed");
    memset(host_buf, 0xa5 ^ args->index, args->size);
    (void)mlock(host_buf, args->size);

    struct dsa_completion_record_aligned completion;
    if (args->do_write)
        submit_dsa_memmove(wq_fd, args->bar_ptr, host_buf, args->size, &completion);
    else
        submit_dsa_memmove(wq_fd, host_buf, args->bar_ptr, args->size, &completion);

    pthread_barrier_wait(args->start_barrier);
    for (int iter = 0; iter < args->iters; ++iter) {
        if (args->do_write)
            submit_dsa_memmove(wq_fd, args->bar_ptr, host_buf, args->size, &completion);
        else
            submit_dsa_memmove(wq_fd, host_buf, args->bar_ptr, args->size, &completion);
    }
    pthread_barrier_wait(args->end_barrier);

    munlock(host_buf, args->size);
    free(host_buf);
    close(wq_fd);
    return NULL;
}

static double run_dsa_test(void *bar_ptr, size_t worker_size, bool write_test)
{
    pthread_barrier_t start_barrier;
    pthread_barrier_t end_barrier;
    std::vector<pthread_t> threads(num_threads);
    std::vector<struct worker_args> args(num_threads);

    if (worker_size == 0)
        die_msg("per-thread DSA copy size is zero");
    if (worker_size > UINT32_MAX)
        die_msg("per-thread DSA copy size exceeds DSA descriptor xfer_size limit");

    if (pthread_barrier_init(&start_barrier, NULL, num_threads + 1) != 0)
        die_msg("pthread_barrier_init failed");
    if (pthread_barrier_init(&end_barrier, NULL, num_threads + 1) != 0)
        die_msg("pthread_barrier_init failed");

    for (int i = 0; i < num_threads; ++i) {
        args[i].index = i;
        args[i].cpu_id = first_cpu >= 0 ? first_cpu + i : -1;
        args[i].iters = num_iters;
        args[i].do_write = write_test;
        args[i].dsa_wq_path = dsa_wq_path.c_str();
        args[i].bar_ptr = (void *)((uintptr_t)bar_ptr + (uintptr_t)i * worker_size);
        args[i].size = worker_size;
        args[i].start_barrier = &start_barrier;
        args[i].end_barrier = &end_barrier;
        if (pthread_create(&threads[i], NULL, dsa_worker, &args[i]) != 0)
            die_errno("pthread_create");
    }

    struct timespec beg, end;
    pthread_barrier_wait(&start_barrier);
    clock_gettime(MYCLOCK, &beg);
    pthread_barrier_wait(&end_barrier);
    clock_gettime(MYCLOCK, &end);

    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);
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
    int c;
    while ((c = getopt(argc, argv, "f:q:s:i:t:o:p:RWBh")) != -1) {
        switch (c) {
            case 'f':
                resource_path = optarg;
                break;
            case 'q':
                dsa_wq_path = optarg;
                break;
            case 's':
                per_thread_size = parse_size(optarg, "per-thread DSA copy size");
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
    if (num_iters <= 0)
        die_msg("iterations must be positive");
    if (num_threads < 0)
        die_msg("thread count must be non-negative");
    if (num_threads == 0)
        num_threads = get_online_cpus();
    if (num_threads <= 0)
        die_msg("thread count must be positive");

    long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0)
        die_errno("sysconf(_SC_PAGESIZE)");
    const size_t page_size = (size_t)page_size_long;
    const off_t map_offset = (off_t)PAGE_ROUND_DOWN(bar_offset, page_size);
    const size_t page_delta = bar_offset - (size_t)map_offset;
    const size_t total_stream_size = per_thread_size * (size_t)num_threads;
    if (num_threads != 0 && total_stream_size / (size_t)num_threads != per_thread_size)
        die_msg("total stream size overflow");
    const size_t map_size = PAGE_ROUND_UP(page_delta + total_stream_size, page_size);

    int open_flags = do_write ? O_RDWR : O_RDONLY;
#ifdef O_SYNC
    open_flags |= O_SYNC;
#endif
    int fd = open(resource_path.c_str(), open_flags);
    if (fd < 0)
        die_errno("open resource");

    int prot = PROT_READ | (do_write ? PROT_WRITE : 0);
    void *map_base = mmap(NULL, map_size, prot, MAP_SHARED, fd, map_offset);
    if (map_base == MAP_FAILED)
        die_errno("mmap");

    void *bar_ptr = (void *)((uintptr_t)map_base + page_delta);

    std::cout << "resource file: " << resource_path << std::endl;
    std::cout << "DSA work queue: " << dsa_wq_path << std::endl;
    std::cout << "BAR offset: 0x" << std::hex << bar_offset << std::dec << std::endl;
    std::cout << "mapped size: " << map_size << std::endl;
    std::cout << "per-thread DSA copy size: " << per_thread_size << std::endl;
    std::cout << "total DSA copy size: " << total_stream_size << std::endl;
    std::cout << "threads: " << num_threads << std::endl;
    std::cout << "iterations: " << num_iters << std::endl;
    std::cout << "user-space mapped pointer: " << bar_ptr << std::endl;
    if (first_cpu >= 0)
        std::cout << "pinned CPU range: " << first_cpu << "-" << (first_cpu + num_threads - 1) << std::endl;

    printf("Test \t\t Threads \t Size(B) \t Avg.BW(MB/s)\n");
    if (do_write) {
        double bw = run_dsa_test(bar_ptr, per_thread_size, true);
        printf("pcibar_dsa_write_bw \t %7d \t %8zu \t %12.2f\n", num_threads, total_stream_size, bw);
    }
    if (do_read) {
        double bw = run_dsa_test(bar_ptr, per_thread_size, false);
        printf("pcibar_dsa_read_bw \t %7d \t %8zu \t %12.2f\n", num_threads, total_stream_size, bw);
    }

    if (munmap(map_base, map_size) != 0)
        die_errno("munmap");
    close(fd);

    return EXIT_SUCCESS;
}
