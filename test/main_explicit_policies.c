#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/allocator_explicit.h"

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

/* same coalescing scenario used throughout: A B C D adjacent, free A, C,
 * then B — should merge all three into one block at A's address, and
 * (explicit-list-specific) leave the free list with exactly that one
 * node, no stale duplicates from the absorbed blocks. */
static void run_coalescing_test(void) {
    allocator_init(1024, FIRST_FIT);

    void *a_ptr = custom_malloc(40);
    void *b_ptr = custom_malloc(40);
    void *c_ptr = custom_malloc(40);
    void *d_ptr = custom_malloc(40);

    memset(d_ptr, 0xEE, 40);

    custom_free(a_ptr);
    custom_free(c_ptr);
    custom_free(b_ptr);

    void *merged = custom_malloc(150);
    CHECK(merged == a_ptr,
          "coalesce-test: merged A+B+C allocation reuses A's original address");

    int d_ok = 1;
    for (int i = 0; i < 40; i++)
        if (((unsigned char *)d_ptr)[i] != 0xEE) d_ok = 0;
    CHECK(d_ok, "coalesce-test: block D's data survived the surrounding merges untouched");

    allocator_destroy();
}

/* explicit-list-specific: after the merge above, the free list should
 * have exactly one node consumed by the 150-byte alloc — a further
 * allocation must not land inside that live block, which would happen
 * if a stale/duplicate free-list node had been left behind. */
static void run_list_integrity_test(void) {
    allocator_init(1024, FIRST_FIT);

    void *a_ptr = custom_malloc(40);
    void *b_ptr = custom_malloc(40);
    custom_free(a_ptr);
    custom_free(b_ptr);

    void *taken = custom_malloc(50);
    CHECK(taken == a_ptr, "list-test: merged free block is found and reused correctly");

    void *next_alloc = custom_malloc(16);
    CHECK((char *)next_alloc >= (char *)taken + 40,
          "list-test: no stale free-list node causes overlap with the live allocation");

    allocator_destroy();
}

/* BEST_FIT / WORST_FIT: three free blocks kept NON-ADJACENT via live
 * spacer blocks, so they cannot coalesce and collapse the test into a
 * single trivial candidate. */
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

/* NEXT_FIT wraparound: push last_found_slot toward the tail of the free
 * list via one allocation, then free an EARLY block that only pass 2
 * (walking from free_list_head) could discover. */
static void run_next_fit_wraparound_test(void) {
    allocator_init(1024, NEXT_FIT);

    void *early = custom_malloc(40);
    void *mid1  = custom_malloc(40);
    void *mid2  = custom_malloc(40);
    void *late  = custom_malloc(40);

    custom_free(late);
    void *reused_late = custom_malloc(40);
    CHECK(reused_late == late,
          "next-fit setup: late block is found and reused, last_found_slot advances");

    custom_free(early);

    void *result = custom_malloc(40);
    CHECK(result == early,
          "next-fit: wraparound pass finds the early free block after the forward pass finds nothing");

    (void)mid1; (void)mid2;
    allocator_destroy();
}

/* explicit-list-specific: last_found_slot must not go stale when the
 * node it points at gets absorbed by coalescing. Free a block so
 * NEXT_FIT points last_found_slot at it, then free its neighbor so it
 * gets merged away — a subsequent search must not read through a
 * dangling pointer. */
static void run_next_fit_coalesce_interaction_test(void) {
    allocator_init(1024, NEXT_FIT);

    void *x = custom_malloc(40);
    void *y = custom_malloc(40);
    void *z = custom_malloc(40);

    custom_free(x); /* last_found_slot has never been set yet (still NULL) */
    void *found_x = custom_malloc(40); /* NEXT_FIT finds x, sets last_found_slot = x->next (NULL) */
    CHECK(found_x == x, "next-fit/coalesce setup: x is found and reused");

    custom_free(x); /* x free again */
    custom_free(y); /* y's left neighbor x is free -> merges; if last_found_slot pointed
                      * at y or x mid-merge, list_remove's redirect must keep it valid */

    void *result = custom_malloc(70); /* only satisfiable by the merged x+y block */
    CHECK(result == x,
          "next-fit/coalesce: search after a merge involving a tracked node does not crash "
          "or misbehave, and correctly finds the merged block");

    (void)z;
    allocator_destroy();
}

int main(void) {
    run_regression_tests();
    run_coalescing_test();
    run_list_integrity_test();
    run_best_fit_test();
    run_worst_fit_test();
    run_next_fit_wraparound_test();
    run_next_fit_coalesce_interaction_test();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}