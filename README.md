# Custom Memory Allocator in C

A from-scratch implementation of `malloc`/`free` (as `custom_malloc`/`custom_free`),
built in two independent versions — an **implicit free list** and an
**explicit free list** — each supporting alignment, block splitting,
recursive bidirectional coalescing, four selectable allocation strategies,
and `mmap`-backed memory. Verified clean under AddressSanitizer/UBSan
throughout, and benchmarked against real-allocator-style workloads with
findings traced down to CPU cache-miss counters.

## Why two implementations

The two designs represent a genuine engineering trade-off, not just two ways
of doing the same thing:

| | Implicit list | Explicit list |
|---|---|---|
| Free block search | Walks *every* block (free or used), in physical memory order | Walks *only* free blocks, via a doubly-linked list |
| Per-block overhead | Header + footer only | Header + footer + `next`/`prev` pointers |
| Coalescing | Pure arithmetic (grow a size field) | Arithmetic **and** list unlinking of absorbed nodes |
| Best case | Simple, no list-consistency bugs possible | Search cost independent of used-block count |

Both were built incrementally: bump allocator → header-based tracking →
free-list search → splitting → coalescing → four fit policies → `mmap`
backing. Each stage was tested and verified working before the next was
added.

## Project structure

```
src/
  allocator.c              implicit list, malloc()-backed (earlier stage, kept for reference)
  allocator_mmap.c          implicit list, mmap()-backed (current/final version)
  allocator_explicit.c     explicit list, malloc()-backed (earlier stage, kept for reference)
  allocator_explicit_mmap.c explicit list, mmap()-backed (current/final version)
  allocator.h              shared header for the implicit-list variants
  allocator_explicit.h     shared header for the explicit-list variants
test/
  main_policies.c          full test suite for the implicit-list allocator
  main_explicit_policies.c full test suite for the explicit-list allocator
bench/
  bench_real_implicit.c    sh6bench/alloc-test-style benchmarks, implicit list
  bench_real_explicit.c    sh6bench/alloc-test-style benchmarks, explicit list
```

The `_mmap` files are the final, current versions — everything in this
README (bug history, benchmark results, design decisions) applies to them.
The non-`_mmap` files are kept as a visible snapshot of the pre-`mmap`
stage of the project rather than relying solely on git history to show
that progression; they're functionally superseded, not maintained in
parallel.

## Memory layout

```
[header][payload][footer][header][payload][footer]...
```

Every block carries a **header** (`size`, `free` flag, and — explicit list
only — `next`/`prev`) and a **footer** (`size`, duplicated). The footer
exists specifically to make **backward coalescing** possible: given a
block's address, there's no way to compute where the *previous* physical
block starts without first reading that neighbor's own size — which is
exactly what the footer, sitting at a fixed offset just before the current
block, provides. All allocations are 16-byte aligned.

## Splitting and coalescing

- **Splitting**: when a matched free block is significantly larger than
  the request, the leftover is carved into its own block (with its own
  header/footer) rather than handed over whole — but only if the leftover
  is large enough to be a *usable* block on its own (a threshold check;
  otherwise the whole block is handed over slightly oversized rather than
  creating an unusable sliver).
- **Coalescing**: on free, both the physically-next and physically-previous
  blocks are checked, and merged if free — recursively, so a chain of
  adjacent free blocks collapses into one in a single `custom_free` call.

## Fit policies

Implemented once per list structure (search logic only — nothing else
changes), selected at runtime via `allocator_init(size, policy)`:

- **`FIRST_FIT`** — return the first block that fits.
- **`BEST_FIT`** — walk the whole list, return the tightest fit.
- **`WORST_FIT`** — walk the whole list, return the largest fit.
- **`NEXT_FIT`** — resume searching from where the last successful search
  left off, wrapping around to the start if nothing is found before the
  end of the pool/list.

## Backing store

Both allocators get their pool directly from `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`,
rounded up to the system page size, rather than from libc's own `malloc` —
closing the slightly awkward "an allocator built on top of the allocator
it's replacing" gap from earlier versions of the project.

---

## Notable bugs found and fixed along the way

These are worth knowing because each one represents a real, general C
pitfall, not just a typo:

- **Arithmetic on `void*`** — not valid in standard C; every byte-offset
  computation needs a `char*` (or explicit cast) first.
- **Footer→header address arithmetic off-by-one** — the correct formula is
  `footer_address - payload_size - HEADER_SIZE`; an early version omitted
  the `- HEADER_SIZE` term, silently misreading the wrong block during
  backward coalescing. Found via a test that checked the *address* a
  merged allocation landed on, not just whether it succeeded.
- **Unsigned sentinel bug in `findWorstFit`** — initializing a `size_t`
  "worst so far" tracker to a faked `INT_MIN` wraps around to a huge
  positive number (unsigned types can't hold negatives), silently making
  the comparison always false. Fixed by using `NULL`-check-based
  initialization instead of a numeric sentinel.
- **Stale list pointers during coalescing (explicit list)** — when a free
  block gets absorbed by a merge, anything else pointing at it (like
  `NEXT_FIT`'s resume pointer) needs to be redirected, or it dangles.
  Centralizing this redirect inside `list_remove` (rather than
  duplicating it at every call site) meant one fix covered every removal
  path — allocation, forward merge, and backward merge — automatically.

---

## Benchmark methodology

Two real-allocator-style workloads, re-implemented from the well-documented
*patterns* of classic allocator benchmarks (not copied source):

- **sh6bench-style** (from SmartHeap's benchmark suite): growing rounds of
  allocations, each round freed in **reverse order**. This is close to a
  best-case pattern for coalescing, since each newly-freed block's right
  neighbor was freed one step earlier.
- **alloc-test-style** (from mimalloc-bench): a fixed-size working set,
  repeatedly freeing a random live slot and allocating something new in
  its place — sustained steady-state churn, closer to a real long-running
  program's heap behavior than a one-shot allocate-everything pattern.

All timing is **best-of-5 trials** (minimum, not average — filters OS
scheduling noise) with one discarded warm-up run beforehand. Every trial
within a policy uses a fixed RNG seed, so all four policies see an
identical request sequence — the only variable is which block gets chosen.

## Results — sh6bench-style (reverse-order free stress)

20 rounds, up to ~8,100 objects/round, sizes up to 640 bytes:

| Policy | Implicit (s) | Explicit (s) | Free blocks (both) | Ext. frag ratio (both) |
|---|---|---|---|---|
| FIRST_FIT | 1.176 | 0.0016 | 1 | 1.0000 |
| BEST_FIT | 1.196 | 0.0022 | 1 | 1.0000 |
| WORST_FIT | 1.148 | 0.0019 | 1 | 1.0000 |
| NEXT_FIT | 0.402 | 0.0016 | 1 | 1.0000 |

**Every policy, on both structures, collapses to exactly one free block**
after the full reverse-order-free stress test. This is about as strong a
real-workload confirmation of coalescing correctness as a benchmark can
give — the theory (reverse-free order is coalescing-friendly, since each
freed block's neighbor was already freed) held up perfectly.

## Results — alloc-test-style (steady-state churn)

Working set of 20,000 live objects, 1,000,000 churn iterations:

| Policy | Implicit (s) | Explicit (s) | Free blocks (I / E) | Ext. frag (I / E) |
|---|---|---|---|---|
| FIRST_FIT | 35.15 | 0.205 | 1256 / 7309 | 0.663 / 0.001 |
| BEST_FIT | 88.34 | 0.327 | 275 / 168 | 0.335 / 0.443 |
| WORST_FIT | 139.92 | 108.16 | 9765 / 9637 | 0.0002 / 0.0002 |
| NEXT_FIT | 1.291 | 2.428 | 4009 / 6018 | 0.002 / 0.001 |

Several distinct, mechanism-confirmed findings:

**WORST_FIT is catastrophic, and the cost is compounding, not linear.**
Scaling the workload 3.33x (300K → 1M iterations) increased WORST_FIT's
runtime by ~9-10x — consistent with roughly quadratic behavior. The
mechanism: worst-fit deliberately grabs oversized blocks, which triggers a
split almost every call, continuously feeding new remainder nodes into the
very free list it must fully scan (no early exit) on every subsequent
search. This is visible directly in the free-block-count column — worst-fit
ends every run with by far the largest free list.

**Explicit's structural advantage depends on the free list staying small
— and worst-fit destroys that precondition.** For FIRST_FIT, explicit is
~170x faster than implicit; for BEST_FIT, ~270x faster. For WORST_FIT, only
~1.3x faster. Explicit's whole advantage ("only walk free blocks, skip
used ones") stops mattering once nearly every block in the pool *is* a
free-list member — which is exactly what worst-fit's constant splitting
produces.

**NEXT_FIT is dramatically faster on implicit, but slower on explicit than
first-fit — a genuinely counterintuitive result, investigated below.**

## Deep dive: why is NEXT_FIT slower on explicit than implicit?

This didn't have an obvious answer, and the investigation is worth
including because the first hypothesis turned out to be *incomplete*, and
was overturned with harder evidence rather than left standing.

**Hypothesis 1 (partially right, but not the main cause):** next-fit's
"resume where I left off" design assumes a *spatial* ordering — true for
implicit (address order, stable over a block's lifetime) but false for
explicit, where `list_push_front` always inserts the most-recently-freed
block at the *head*, regardless of physical address. Instrumenting the
search confirmed this is real: explicit's wraparound rate (4.77%) is
~3.8x higher than implicit's (1.26%). But a <5%-of-calls effect is too
small to explain a 1.8x absolute timing gap on its own.

**Hypothesis 2 (also real, also insufficient alone):** maybe explicit
visits more nodes per call. Measured directly — explicit actually visits
*fewer* total nodes (268.6M vs 536.3M) than implicit over the same run,
which if anything predicts explicit should be *faster*, not slower.

**The actual dominant cause, confirmed with `perf`:** cache-miss rate per
node visited.

| | Total nodes visited | Cache misses (`perf stat`) | Misses per node |
|---|---|---|---|
| Implicit | 593,306,344 | 203,872,918 | 0.344 |
| Explicit | 268,653,617 | 415,964,460 | 1.548 |

Explicit does **less than half the nominal work** but pays a **~4.5x
higher cache-miss rate per node**, and that dominates. The reason:
implicit's walk (`curr_ptr += size`) is sequential through memory, which
the CPU's hardware prefetcher predicts well. Explicit's walk follows
`->next` pointers whose *list* order (LIFO by free time) has no
relationship to their *physical* memory order — each dereference can jump
anywhere across the pool, defeating the prefetcher on nearly every step.
This is the textbook cost of pointer-chasing through a memory-scattered
linked list, and it's a stronger, more complete explanation than either
hypothesis alone.

## Known limitations

- **No `realloc`/`calloc`** — not yet implemented in either version. The
  interesting piece left undone: `realloc` could grow in place by
  absorbing a free right-neighbor (reusing the same merge mechanics as
  `custom_free`'s forward coalescing) rather than always
  copy-and-free, given coalescing infrastructure already exists.
- **No thread safety** — single-threaded by design; a shared mutex around
  the free list (or per-thread arenas) would be the natural next step.
- **No segregated size-class buckets** — considered and designed on paper,
  deliberately deprioritized in favor of depth on the two structures
  actually built (see project history for the reasoning).
- **Internal-fragmentation measurement is incomplete** — the benchmark's
  internal-frag metric only captures alignment-rounding waste, not the
  unsplit-leftover waste from the split threshold (which *is*
  policy-dependent but wasn't instrumented).
- **External fragmentation is unfixable by any of these policies** — an
  earlier, smaller-scale checkerboard-fill test confirmed a large
  contiguous allocation fails identically across all four policies after
  fragmenting the pool, even with plenty of free memory in aggregate.
  Fit policy changes *which* free block gets picked; only compaction
  (not implemented) can fix genuine external fragmentation.

## Build

```
gcc -Wall -Wextra -fsanitize=address,undefined -g -o test_policies src/allocator_mmap.c test/main_policies.c
./test_policies

gcc -Wall -Wextra -fsanitize=address,undefined -g -o test_explicit_policies src/allocator_explicit_mmap.c test/main_explicit_policies.c
./test_explicit_policies

gcc -O2 -o bench_real_implicit src/allocator_mmap.c bench/bench_real_implicit.c
./bench_real_implicit

gcc -O2 -o bench_real_explicit src/allocator_explicit_mmap.c bench/bench_real_explicit.c
./bench_real_explicit
```
