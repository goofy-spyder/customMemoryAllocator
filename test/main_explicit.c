#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/allocator_explicit.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else      { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void run_basic_and_split_tests(void) {
    allocator_init(1024);

    int *a = (int *)custom_malloc(sizeof(int) * 4);
    CHECK(a != NULL, "custom_malloc returns non-NULL for a normal request");
    for (int i = 0; i < 4; i++) a[i] = i * 10;
    int data_ok = 1;
    for (int i = 0; i < 4; i++) if (a[i] != i * 10) data_ok = 0;
    CHECK(data_ok, "data written through the returned pointer round-trips correctly");

    void *before_free_bump_addr = custom_malloc(32);
    custom_free(before_free_bump_addr);
    void *reused = custom_malloc(32);
    CHECK(reused == before_free_bump_addr,
          "freeing a block and re-requesting the same size reuses that exact slot");

    void *e = custom_malloc(16);
    custom_free(e);
    custom_free(e);
    void *f = custom_malloc(16);
    CHECK(f != NULL, "allocator still functions correctly after a double-free warning");

    CHECK(custom_malloc(0) == NULL, "zero-size request returns NULL");
    CHECK(custom_malloc(100000) == NULL, "oversized request (bigger than pool) returns NULL");

    custom_free(NULL);
    CHECK(1, "custom_free(NULL) does not crash");

    allocator_destroy();

    /* ---- splitting, fresh pool ---- */
    allocator_init(1024);

    void *big = custom_malloc(256);
    custom_free(big);
    void *small1 = custom_malloc(16);
    CHECK(small1 == big,
          "split-test: small request reuses the start of the freed 256-byte block");
    memset(small1, 0xAB, 16);
    void *small2 = custom_malloc(16);
    CHECK(small2 != small1 && (char *)small2 < (char *)big + 256,
          "split-test: second allocation was satisfied from the leftover remainder");
    memset(small2, 0xCD, 16);
    CHECK(((unsigned char *)small1)[0] == 0xAB,
          "split-test: writing into the second block did not corrupt the first");

    allocator_destroy();
}

/* same coalescing scenario as the implicit-list test: A B C D adjacent,
 * free A, C, then B — should trigger a double-direction merge into one
 * block starting at A's address, with the free list correctly reflecting
 * exactly one node afterward instead of three stale/duplicate entries. */
static void run_coalescing_test(void) {
    allocator_init(1024);

    void *a_ptr = custom_malloc(40);
    void *b_ptr = custom_malloc(40);
    void *c_ptr = custom_malloc(40);
    void *d_ptr = custom_malloc(40);

    CHECK(a_ptr && b_ptr && c_ptr && d_ptr,
          "coalesce-test: all four initial 40-byte allocations succeed");

    memset(d_ptr, 0xEE, 40);

    custom_free(a_ptr);
    custom_free(c_ptr);
    custom_free(b_ptr);

    void *merged = custom_malloc(150);
    CHECK(merged != NULL,
          "coalesce-test: a request too big for any single freed block succeeds "
          "after A+B+C were merged");
    CHECK(merged == a_ptr,
          "coalesce-test: the merged allocation reuses A's original address "
          "(proves backward merge into A actually happened)");

    int d_ok = 1;
    for (int i = 0; i < 40; i++)
        if (((unsigned char *)d_ptr)[i] != 0xEE) d_ok = 0;
    CHECK(d_ok, "coalesce-test: block D's data survived the surrounding merges untouched");

    memset(merged, 0x11, 150);
    d_ok = 1;
    for (int i = 0; i < 40; i++)
        if (((unsigned char *)d_ptr)[i] != 0xEE) d_ok = 0;
    CHECK(d_ok, "coalesce-test: writing the full merged block did not overrun into D");

    allocator_destroy();
}

/* explicit-list-specific: after the merge above, custom_malloc(150) should
 * have pulled the ONLY node out of the free list (list_remove happened
 * correctly during merging, no stale duplicate nodes left behind). A
 * subsequent request for any size should now be forced to bump, since
 * nothing legitimate should remain in the free list. */
static void run_list_integrity_test(void) {
    allocator_init(1024);

    void *a_ptr = custom_malloc(40);
    void *b_ptr = custom_malloc(40);
    custom_free(a_ptr);
    custom_free(b_ptr); /* merges into one node at a_ptr's address */

    void *taken = custom_malloc(50); /* should consume the merged free-list node entirely */
    CHECK(taken == a_ptr, "list-test: merged free block is found and reused correctly");

    void *next_alloc = custom_malloc(16);
    /* if the free list still had a stale/duplicate node, this could
     * incorrectly return an address inside the block we just handed out
     * above instead of bumping past it */
    CHECK((char *)next_alloc >= (char *)taken + 40,
          "list-test: no stale free-list node causes overlap with the live allocation");

    allocator_destroy();
}

int main(void) {
    run_basic_and_split_tests();
    run_coalescing_test();
    run_list_integrity_test();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}