#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/allocator.h"

#define POOL_SIZE (16 * 1024 * 1024)
#define N 50000
#define FREE_CHANCE 40 /* percent chance per iteration of freeing a live pointer */
#define NUM_TRIALS 5
#define ALLIGNMENT 16
#define ALLIGN_UP(n) (((n) + (ALLIGNMENT - 1)) & ~(ALLIGNMENT - 1))

static const char *policy_name(uint8_t p) {
    switch (p) {
        case FIRST_FIT: return "FIRST_FIT";
        case BEST_FIT:  return "BEST_FIT";
        case WORST_FIT: return "WORST_FIT";
        case NEXT_FIT:  return "NEXT_FIT";
    }
    return "UNKNOWN";
}

/* Real allocation workloads skew heavily toward small, short-lived
 * objects with occasional larger ones — not uniform random sizes.
 * Roughly: 80% small (8-128B), 15% medium (128-512B), 5% large
 * (512B-4KB). This is the same rough shape used by allocator
 * microbenchmarks like mimalloc-bench's synthetic tests. */
static size_t random_alloc_size(void) {
    int r = rand() % 100;
    if (r < 80) return (rand() % 121) + 8;
    if (r < 95) return (rand() % 385) + 128;
    return (rand() % 3585) + 512;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    double seconds;
    double internal_frag_pct;
    double external_frag_ratio;
    size_t free_block_count;
} bench_result_t;

/* runs the workload once; `timed` controls whether this run's duration
 * is meaningful (a warm-up run still exercises the allocator fully but
 * its timing is discarded, so first-touch page-fault costs don't
 * contaminate the measured trials) */
static bench_result_t run_once(uint8_t policy, int timed) {
    void *ptrs[N];
    for (int i = 0; i < N; i++) ptrs[i] = NULL;

    allocator_init(POOL_SIZE, policy);
    srand(42); /* identical request sequence every trial, every policy */

    size_t sum_requested = 0;
    size_t sum_aligned = 0;

    double start = timed ? now_seconds() : 0.0;
    for (int i = 0; i < N; i++) {
        size_t sz = random_alloc_size();
        void *p = custom_malloc(sz);
        ptrs[i] = p;
        if (p) {
            sum_requested += sz;
            sum_aligned += ALLIGN_UP(sz);
        }

        if (i > 50 && (rand() % 100) < FREE_CHANCE) {
            int victim = rand() % i;
            if (ptrs[victim]) {
                custom_free(ptrs[victim]);
                ptrs[victim] = NULL;
            }
        }
    }
    double elapsed = timed ? (now_seconds() - start) : 0.0;

    size_t total_free = allocator_total_free();
    size_t largest_free = allocator_largest_free();
    size_t free_blocks = allocator_free_block_count();
    double external_ratio = total_free > 0 ? (double)largest_free / (double)total_free : 1.0;

    for (int i = 0; i < N; i++) if (ptrs[i]) custom_free(ptrs[i]);
    allocator_destroy();

    bench_result_t result;
    result.seconds = elapsed;
    result.internal_frag_pct = sum_aligned > 0
        ? 100.0 * (double)(sum_aligned - sum_requested) / (double)sum_aligned
        : 0.0;
    result.external_frag_ratio = external_ratio;
    result.free_block_count = free_blocks;
    return result;
}

/* best-of-N: run the identical deterministic workload NUM_TRIALS times
 * and keep the fastest — standard practice for filtering out OS
 * scheduling jitter and transient interference rather than averaging
 * it in. frag/count metrics are identical every trial (the workload is
 * fully deterministic), so they're just taken from the last trial. */
static bench_result_t run_timing_workload(uint8_t policy) {
    run_once(policy, 0); /* warm-up, discarded */

    bench_result_t best;
    best.seconds = -1.0;
    bench_result_t last;
    for (int t = 0; t < NUM_TRIALS; t++) {
        last = run_once(policy, 1);
        if (best.seconds < 0.0 || last.seconds < best.seconds) best.seconds = last.seconds;
    }
    last.seconds = best.seconds;
    return last;
}

int main(void) {
    uint8_t policies[] = { FIRST_FIT, BEST_FIT, WORST_FIT, NEXT_FIT };
    int num_policies = 4;

    printf("=== implicit list - benchmark (N=%d allocs, best-of-%d trials) ===\n", N, NUM_TRIALS);
    printf("%-12s %10s %13s %14s %22s\n",
           "policy", "seconds", "free blocks", "internal frag%", "external frag ratio");

    for (int i = 0; i < num_policies; i++) {
        bench_result_t r = run_timing_workload(policies[i]);
        printf("%-12s %10.4f %13zu %13.2f%% %22.4f\n",
               policy_name(policies[i]), r.seconds, r.free_block_count,
               r.internal_frag_pct, r.external_frag_ratio);
    }

    printf("\nnotes:\n");
    printf("- timing is best-of-%d trials (min), not average, to filter OS jitter.\n", NUM_TRIALS);
    printf("- one untimed warm-up run precedes the measured trials.\n");
    printf("- size distribution is bimodal-ish (80%% 8-128B, 15%% 128-512B, 5%% 512B-4KB),\n");
    printf("  approximating real allocation patterns rather than uniform random.\n");
    printf("- free blocks = live free-list length at end of the workload. a policy\n");
    printf("  that keeps splitting off small leftovers grows this over time, which\n");
    printf("  directly increases the cost of every subsequent search.\n");
    printf("- internal frag %% depends only on ALLIGN_UP rounding of requested sizes,\n");
    printf("  identical across policies by construction -- included as a baseline.\n");
    printf("  it does NOT capture unsplit-leftover waste, which IS policy-dependent\n");
    printf("  but isn't measured here (would need per-block size introspection).\n");

    return 0;
}