#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/allocator.h"

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

    char *b = (char *)custom_malloc(64);
    CHECK(b != NULL, "second custom_malloc returns non-NULL");
    b[0] = 'X'; b[63] = 'Y';
    CHECK(a[0] == 0 && a[3] == 30, "writing into block b did not disturb block a");

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

/* ============================================================
 * COALESCING — the new test.
 *
 * Layout: four adjacent blocks A, B, C, D, each requesting 40 bytes.
 * We free A, then C, then B — freeing B should trigger BOTH a forward
 * merge (B absorbs C, which is already free) AND a backward merge
 * (the B+C block then gets absorbed into A, which is also already
 * free) in a single custom_free call. D is never touched and must
 * remain completely untouched and independently usable throughout.
 *
 * Proof that the merge actually happened, not just "didn't crash":
 * request a block bigger than any single 40-byte slot could hold, but
 * small enough to fit in A+B+C combined. If coalescing is broken, this
 * allocation either fails (no free block big enough) or gets satisfied
 * by bumping past D instead of reusing A's address.
 * ============================================================ */
static void run_coalescing_test(void) {
    allocator_init(1024);

    void *a_ptr = custom_malloc(40);
    void *b_ptr = custom_malloc(40);
    void *c_ptr = custom_malloc(40);
    void *d_ptr = custom_malloc(40);

    CHECK(a_ptr && b_ptr && c_ptr && d_ptr,
          "coalesce-test: all four initial 40-byte allocations succeed");

    memset(d_ptr, 0xEE, 40); /* D's data — must survive everything below untouched */

    custom_free(a_ptr);
    custom_free(c_ptr);
    custom_free(b_ptr); /* should trigger a double-direction merge: A + B + C -> one block */

    /* This request is bigger than any single 40-byte(-aligned) block, but
     * fits in the merged A+B+C region. If it succeeds AND lands at A's
     * old address, coalescing genuinely combined all three. */
    void *merged = custom_malloc(150);
    CHECK(merged != NULL,
          "coalesce-test: a request too big for any single freed block succeeds "
          "after A+B+C were merged");
    CHECK(merged == a_ptr,
          "coalesce-test: the merged allocation reuses A's original address "
          "(proves backward merge into A actually happened)");

    /* D must be completely unaffected by any of the merging around it */
    int d_ok = 1;
    for (int i = 0; i < 40; i++)
        if (((unsigned char *)d_ptr)[i] != 0xEE) d_ok = 0;
    CHECK(d_ok, "coalesce-test: block D's data survived the surrounding merges untouched");

    /* writing across the full merged region should be safe and not
     * corrupt D, which sits immediately after it */
    memset(merged, 0x11, 150);
    d_ok = 1;
    for (int i = 0; i < 40; i++)
        if (((unsigned char *)d_ptr)[i] != 0xEE) d_ok = 0;
    CHECK(d_ok, "coalesce-test: writing the full merged block did not overrun into D");

    allocator_destroy();
}

int main(void) {
    run_basic_and_split_tests();
    run_coalescing_test();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}