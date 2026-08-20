#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEMORY_SIZE       (16u * 1024u * 1024u) * 4
#define MEMORY_PASSES     32u * 4
#define MEMCPY_PASSES     128u * 4
#define HASH_PASSES       16u * 4
#define SORT_COUNT        500000u * 4
#define ALLOC_COUNT       100000u * 4
#define INTEGER_ITERS     50000000ULL * 4
#define FLOAT_ITERS       50000000ULL * 4

#define MIB (1024.0 * 1024.0)

/*
 * results are written here so the compiler doesn't optimize them away
 */
static volatile uint64_t sink_u64;
static volatile double sink_double;


/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime(CLOCK_MONOTONIC) failed\n");
        exit(EXIT_FAILURE);
    }

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1000000000.0;
}


/* ------------------------------------------------------------------------- */
/* Utilities                                                                 */
/* ------------------------------------------------------------------------- */

static void *checked_malloc(size_t size)
{
    void *p = malloc(size);

    if (p == NULL) {
        fprintf(stderr, "malloc(%lu) failed\n",
                (unsigned long)size);
        exit(EXIT_FAILURE);
    }

    return p;
}


static uint32_t prng_state = UINT32_C(0x12345678);

static uint32_t prng_next(void)
{
    uint32_t x = prng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    prng_state = x;
    return x;
}


/* ------------------------------------------------------------------------- */
/* Integer                                                                   */
/* ------------------------------------------------------------------------- */

static void bench_integer(void)
{
    uint64_t x = UINT64_C(0x123456789abcdef0);
    uint64_t i;
    double start;
    double end;
    double elapsed;

    start = now_seconds();

    for (i = 0; i < INTEGER_ITERS; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;

        x += i;
        x *= UINT64_C(0x9e3779b97f4a7c15);
    }

    end = now_seconds();

    sink_u64 = x;
    elapsed = end - start;

    printf("%-12s %8.3f s  %12.2f Miter/s\n",
           "integer",
           elapsed,
           ((double)INTEGER_ITERS / elapsed) / 1000000.0);
}


/* ------------------------------------------------------------------------- */
/* Floating point                                                            */
/* ------------------------------------------------------------------------- */

static void bench_float(void)
{
    double x = 1.000001;
    double y = 0.999999;
    uint64_t i;
    double start;
    double end;
    double elapsed;

    start = now_seconds();

    for (i = 0; i < FLOAT_ITERS; ++i) {
        x = x * y + 0.000001;
        y = y * 0.9999999 + 0.0000001;

        if (x > 2.0)
            x *= 0.5;
    }

    end = now_seconds();

    sink_double = x + y;
    elapsed = end - start;

    printf("%-12s %8.3f s  %12.2f Miter/s\n",
           "float",
           elapsed,
           ((double)FLOAT_ITERS / elapsed) / 1000000.0);
}


/* ------------------------------------------------------------------------- */
/* Generated memory access                                                   */
/* ------------------------------------------------------------------------- */

static void bench_memory(void)
{
    unsigned char *buffer;
    uint64_t sum = 0;
    size_t i;
    unsigned int pass;
    double start;
    double end;
    double elapsed;
    double bytes;

    buffer = checked_malloc(MEMORY_SIZE);

    start = now_seconds();

    for (pass = 0; pass < MEMORY_PASSES; ++pass) {
        for (i = 0; i < MEMORY_SIZE; ++i)
            buffer[i] = (unsigned char)(i + pass);

        for (i = 0; i < MEMORY_SIZE; ++i)
            sum += buffer[i];
    }

    end = now_seconds();

    sink_u64 = sum;
    free(buffer);

    elapsed = end - start;

    bytes = (double)MEMORY_SIZE *
            (double)MEMORY_PASSES *
            2.0;

    printf("%-12s %8.3f s  %12.2f MiB/s\n",
           "memory",
           elapsed,
           (bytes / MIB) / elapsed);
}


/* ------------------------------------------------------------------------- */
/* libc memcpy                                                               */
/* ------------------------------------------------------------------------- */

static void bench_memcpy(void)
{
    unsigned char *src;
    unsigned char *dst;
    unsigned int pass;
    double start;
    double end;
    double elapsed;
    double bytes;

    src = checked_malloc(MEMORY_SIZE);
    dst = checked_malloc(MEMORY_SIZE);

    memset(src, 0x5a, MEMORY_SIZE);
    memset(dst, 0x00, MEMORY_SIZE);

    start = now_seconds();

    for (pass = 0; pass < MEMCPY_PASSES; ++pass)
        memcpy(dst, src, MEMORY_SIZE);

    end = now_seconds();

    sink_u64 = dst[MEMORY_SIZE / 2u];

    free(src);
    free(dst);

    elapsed = end - start;

    bytes = (double)MEMORY_SIZE *
            (double)MEMCPY_PASSES;

    printf("%-12s %8.3f s  %12.2f MiB/s\n",
           "memcpy",
           elapsed,
           (bytes / MIB) / elapsed);
}


/* ------------------------------------------------------------------------- */
/* FNV-1a                                                                    */
/* ------------------------------------------------------------------------- */

static uint64_t fnv1a(const unsigned char *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < size; ++i) {
        hash ^= (uint64_t)data[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}


static void bench_hash(void)
{
    unsigned char *buffer;
    uint64_t hash = 0;
    unsigned int pass;
    size_t i;
    double start;
    double end;
    double elapsed;
    double bytes;

    buffer = checked_malloc(MEMORY_SIZE);

    for (i = 0; i < MEMORY_SIZE; ++i)
        buffer[i] = (unsigned char)prng_next();

    start = now_seconds();

    for (pass = 0; pass < HASH_PASSES; ++pass) {
        hash ^= fnv1a(buffer, MEMORY_SIZE);
    }

    end = now_seconds();

    sink_u64 = hash;
    free(buffer);

    elapsed = end - start;

    bytes = (double)MEMORY_SIZE *
            (double)HASH_PASSES;

    printf("%-12s %8.3f s  %12.2f MiB/s\n",
           "fnv1a",
           elapsed,
           (bytes / MIB) / elapsed);
}


/* ------------------------------------------------------------------------- */
/* libc qsort                                                                */
/* ------------------------------------------------------------------------- */

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;

    if (x < y)
        return -1;

    if (x > y)
        return 1;

    return 0;
}


static void bench_sort(void)
{
    uint32_t *values;
    size_t i;
    double start;
    double end;
    double elapsed;

    values = checked_malloc(
        (size_t)SORT_COUNT * sizeof(*values));

    for (i = 0; i < SORT_COUNT; ++i)
        values[i] = prng_next();

    start = now_seconds();

    qsort(values,
          SORT_COUNT,
          sizeof(*values),
          compare_u32);

    end = now_seconds();

    sink_u64 =
        (uint64_t)values[0] ^
        (uint64_t)values[SORT_COUNT / 2u] ^
        (uint64_t)values[SORT_COUNT - 1u];

    free(values);

    elapsed = end - start;

    printf("%-12s %8.3f s  %12.2f Kitem/s\n",
           "qsort",
           elapsed,
           ((double)SORT_COUNT / elapsed) / 1000.0);
}


/* ------------------------------------------------------------------------- */
/* libc allocator                                                            */
/* ------------------------------------------------------------------------- */

static void bench_alloc(void)
{
    void **ptrs;
    size_t i;
    size_t size;
    double start;
    double end;
    double elapsed;

    ptrs = checked_malloc(
        (size_t)ALLOC_COUNT * sizeof(*ptrs));

    start = now_seconds();

    for (i = 0; i < ALLOC_COUNT; ++i) {
        /*
         * Deliberately vary allocation sizes
         */
        size = 16u + (i % 4096u);

        ptrs[i] = malloc(size);

        if (ptrs[i] == NULL) {
            fprintf(stderr,
                    "allocation failed during malloc benchmark\n");
            exit(EXIT_FAILURE);
        }

        /*
         * Touch the allocation so it cannot be entirely ignored
         */
        ((unsigned char *)ptrs[i])[0] =
            (unsigned char)i;
    }

    for (i = 0; i < ALLOC_COUNT; ++i)
        free(ptrs[i]);

    end = now_seconds();

    free(ptrs);

    elapsed = end - start;

    printf("%-12s %8.3f s  %12.2f Kops/s\n",
           "malloc/free",
           elapsed,
           ((double)ALLOC_COUNT * 2.0 / elapsed) / 1000.0);
}


/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("tinybench 0.1\n");
    printf("==============\n\n");

    printf("compute\n");
    printf("-------\n");

    bench_integer();
    bench_float();
    bench_hash();

    printf("\n");

    printf("memory\n");
    printf("------\n");

    bench_memory();
    bench_memcpy();

    printf("\n");

    printf("libc\n");
    printf("----\n");

    bench_sort();
    bench_alloc();

    /*
        Check the sink values to stop the compiler from removing them
     */
    if (sink_u64 == UINT64_C(0xffffffffffffffff) &&
        sink_double == -1.0) {
        printf("impossible: %llu %f\n",
               (unsigned long long)sink_u64,
               (double)sink_double);
    }

    return EXIT_SUCCESS;
}