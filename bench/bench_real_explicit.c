#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/allocator_explicit.h"

#define POOL_SIZE (256 * 1024 * 1024)
#define NUM_TRIALS 1

static const char *policy_name(uint8_t p) {
    switch (p) {
        case FIRST_FIT: return "FIRST_FIT";
        case BEST_FIT:  return "BEST_FIT";
        case WORST_FIT: return "WORST_FIT";
        case NEXT_FIT:  return "NEXT_FIT";
    }
    return "UNKNOWN";
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    double seconds;
    double external_frag_ratio;
    size_t free_block_count;
} bench_result_t;

#define SH_ROUNDS 20
#define SH_BASE_COUNT 500
#define SH_COUNT_STEP 400

static bench_result_t run_sh6bench(uint8_t policy, int timed) {
    allocator_init(POOL_SIZE, policy);
    srand(1);

    double start = timed ? now_seconds() : 0.0;
    for (int round = 0; round < SH_ROUNDS; round++) {
        int count = SH_BASE_COUNT + round * SH_COUNT_STEP;
        int min_sz = 8 + round * 4;
        int max_sz = 32 + round * 32;

        void **ptrs = malloc(sizeof(void *) * count);
        for (int i = 0; i < count; i++) {
            size_t sz = (size_t)(min_sz + (rand() % (max_sz - min_sz + 1)));
            ptrs[i] = custom_malloc(sz);
        }
        for (int i = count - 1; i >= 0; i--) {
            if (ptrs[i]) custom_free(ptrs[i]);
        }
        free(ptrs);
    }
    double elapsed = timed ? (now_seconds() - start) : 0.0;

    size_t total_free = allocator_total_free();
    size_t largest_free = allocator_largest_free();
    bench_result_t r;
    r.seconds = elapsed;
    r.free_block_count = allocator_free_block_count();
    r.external_frag_ratio = total_free > 0 ? (double)largest_free / (double)total_free : 1.0;

    allocator_destroy();
    return r;
}

#define WORKING_SET 20000
#define CHURN_ITERATIONS 1000000

static bench_result_t run_alloc_test(uint8_t policy, int timed) {
    allocator_init(POOL_SIZE, policy);
    srand(1);

    void **slots = malloc(sizeof(void *) * WORKING_SET);
    for (int i = 0; i < WORKING_SET; i++) {
        size_t sz = (size_t)(rand() % 256) + 8;
        slots[i] = custom_malloc(sz);
    }

    double start = timed ? now_seconds() : 0.0;
    for (int i = 0; i < CHURN_ITERATIONS; i++) {
        int idx = rand() % WORKING_SET;
        if (slots[idx]) custom_free(slots[idx]);
        size_t sz = (size_t)(rand() % 256) + 8;
        slots[idx] = custom_malloc(sz);
    }
    double elapsed = timed ? (now_seconds() - start) : 0.0;

    size_t total_free = allocator_total_free();
    size_t largest_free = allocator_largest_free();
    bench_result_t r;
    r.seconds = elapsed;
    r.free_block_count = allocator_free_block_count();
    r.external_frag_ratio = total_free > 0 ? (double)largest_free / (double)total_free : 1.0;

    for (int i = 0; i < WORKING_SET; i++) if (slots[i]) custom_free(slots[i]);
    free(slots);
    allocator_destroy();
    return r;
}

static bench_result_t best_of(bench_result_t (*fn)(uint8_t, int), uint8_t policy) {
    printf("  [running %s...]\n", policy_name(policy));
    fflush(stdout);
    fn(policy, 0);
    bench_result_t best;
    best.seconds = -1.0;
    bench_result_t last;
    for (int t = 0; t < NUM_TRIALS; t++) {
        last = fn(policy, 1);
        if (best.seconds < 0.0 || last.seconds < best.seconds) best.seconds = last.seconds;
        printf("    trial %d: %.4fs\n", t + 1, last.seconds);
        fflush(stdout);
    }
    last.seconds = best.seconds;
    return last;
}

static void print_results(const char *title, bench_result_t (*fn)(uint8_t, int),
                           uint8_t *policies, int num_policies) {
    printf("=== explicit list - %s (best-of-%d trials) ===\n", title, NUM_TRIALS);
    printf("%-12s %10s %13s %22s\n", "policy", "seconds", "free blocks", "external frag ratio");
    for (int i = 0; i < num_policies; i++) {
        bench_result_t r = best_of(fn, policies[i]);
        printf("%-12s %10.4f %13zu %22.4f\n",
               policy_name(policies[i]), r.seconds, r.free_block_count, r.external_frag_ratio);
        if (policies[i] == NEXT_FIT) {
            size_t p1 = allocator_next_fit_pass1_hits();
            size_t p2 = allocator_next_fit_pass2_hits();
            size_t total = p1 + p2;
            double p2_rate = total > 0 ? 100.0 * (double)p2 / (double)total : 0.0;
            size_t nodes = allocator_next_fit_nodes_visited();
            size_t calls = allocator_next_fit_calls();
            double avg_scan = calls > 0 ? (double)nodes / (double)calls : 0.0;
            printf("  next-fit detail: pass1 hits=%zu, pass2 (wraparound) hits=%zu, "
                   "wraparound rate=%.2f%%\n", p1, p2, p2_rate);
            printf("  next-fit detail: total calls=%zu, total nodes visited=%zu, "
                   "avg scan depth per call=%.3f\n", calls, nodes, avg_scan);
        }
    }
    printf("\n");
}

int main(int argc, char **argv) {
    uint8_t all_policies[] = { FIRST_FIT, BEST_FIT, WORST_FIT, NEXT_FIT };
    uint8_t selected[4];
    int num_selected = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "first") == 0) selected[num_selected++] = FIRST_FIT;
        else if (strcmp(argv[1], "best") == 0) selected[num_selected++] = BEST_FIT;
        else if (strcmp(argv[1], "worst") == 0) selected[num_selected++] = WORST_FIT;
        else if (strcmp(argv[1], "next") == 0) selected[num_selected++] = NEXT_FIT;
        else {
            printf("unknown policy '%s' -- use first|best|worst|next\n", argv[1]);
            return 1;
        }
    } else {
        for (int i = 0; i < 4; i++) selected[i] = all_policies[i];
        num_selected = 4;
    }

    print_results("sh6bench-style (reverse-order free stress)", run_sh6bench, selected, num_selected);
    print_results("alloc-test-style (steady-state churn)", run_alloc_test, selected, num_selected);
    return 0;
}