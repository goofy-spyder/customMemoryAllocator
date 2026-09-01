#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/allocator.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else      { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void run_regression_tests(void) {
    allocator_init(1024, FIRST_FIT);

    int *a = (int *)custom_malloc(sizeof(int) * 4);
    CHECK(a != NULL, "regression: custom_malloc returns non-NULL for a normal request");
    for (int i = 0; i < 4; i++) a[i] = i * 10;
    int data_ok = 1;
    for (int i = 0; i < 4; i++) if (a[i] != i * 10) data_ok = 0;
    CHECK(data_ok, "regression: data written through the returned pointer round-trips correctly");

    void *before_free_bump_addr = custom_malloc(32);
    custom_free(before_free_bump_addr);
    void *reused = custom_malloc(32);
    CHECK(reused == before_free_bump_addr,
          "regression: freeing a block and re-requesting the same size reuses that exact slot");

    void *e = custom_malloc(16);
    custom_free(e);
    custom_free(e);
    void *f = custom_malloc(16);
    CHECK(f != NULL, "regression: allocator still functions correctly after a double-free warning");

    CHECK(custom_malloc(0) == NULL, "regression: zero-size request returns NULL");
    CHECK(custom_malloc(100000) == NULL, "regression: oversized request returns NULL");
    custom_free(NULL);
    CHECK(1, "regression: custom_free(NULL) does not crash");

    allocator_destroy();

    allocator_init(1024, FIRST_FIT);
    void *big = custom_malloc(256);
    custom_free(big);
    void *small1 = custom_malloc(16);
    CHECK(small1 == big, "regression: split-test small request reuses the freed block's start");
    void *small2 = custom_malloc(16);
    CHECK(small2 != small1 && (char *)small2 < (char *)big + 256,
          "regression: split-test remainder is independently found");
    allocator_destroy();
}

/* ------------------------------------------------------------------
 * BEST_FIT / WORST_FIT: three free blocks of different sizes, kept
 * NON-ADJACENT via a live "spacer" block between each pair, so they
 * cannot coalesce into one block and hide the actual policy choice.
 *
 * Layout: [small][spacer1][medium][spacer2][large][anchor]
 * spacer1/spacer2/anchor stay allocated throughout — never freed —
 * so small/medium/large remain three genuinely separate candidates.
 * ------------------------------------------------------------------ */
static void run_best_fit_test(void) {
    allocator_init(1024, BEST_FIT);

    void *small   = custom_malloc(16);
    void *spacer1 = custom_malloc(16);
    void *medium  = custom_malloc(64);
    void *spacer2 = custom_malloc(16);
    void *large   = custom_malloc(256);
    void *anchor  = custom_malloc(16);

    custom_free(small);
    custom_free(medium);
    custom_free(large);

    void *result = custom_malloc(16);
    CHECK(result == small,
          "best-fit: request is satisfied by the tightest-fitting free block, not the largest");

    (void)spacer1; (void)spacer2; (void)anchor;
    allocator_destroy();
}

static void run_worst_fit_test(void) {
    allocator_init(1024, WORST_FIT);

    void *small   = custom_malloc(16);
    void *spacer1 = custom_malloc(16);
    void *medium  = custom_malloc(64);
    void *spacer2 = custom_malloc(16);
    void *large   = custom_malloc(256);
    void *anchor  = custom_malloc(16);

    custom_free(small);
    custom_free(medium);
    custom_free(large);

    void *result = custom_malloc(16);
    CHECK(result == large,
          "worst-fit: request is satisfied by the largest free block, not the tightest");

    (void)spacer1; (void)spacer2; (void)anchor;
    allocator_destroy();
}

static void run_next_fit_wraparound_test(void) {
    allocator_init(1024, NEXT_FIT);

    void *early = custom_malloc(40);
    void *mid1  = custom_malloc(40);
    void *mid2  = custom_malloc(40);
    void *late  = custom_malloc(40);

    custom_free(late);
    void *reused_late = custom_malloc(40);
    CHECK(reused_late == late,
          "next-fit setup: late block is found and reused, last_found_slot now sits near bump_ptr");

    custom_free(early);

    void *result = custom_malloc(40);
    CHECK(result == early,
          "next-fit: wraparound pass finds the early free block after the forward pass finds nothing");

    (void)mid1; (void)mid2;
    allocator_destroy();
}

int main(void) {
    run_regression_tests();
    run_best_fit_test();
    run_worst_fit_test();
    run_next_fit_wraparound_test();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}