#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

enum Policy {
    FIRST_FIT,
    BEST_FIT,
    NEXT_FIT,
    WORST_FIT
};

void allocator_init(size_t init_size, uint8_t pol);
void allocator_destroy(void);
void *custom_malloc(size_t size);
void custom_free(void *ptr);
size_t allocator_total_free(void);
size_t allocator_largest_free(void);
size_t allocator_free_block_count(void);
size_t allocator_next_fit_pass1_hits(void);
size_t allocator_next_fit_pass2_hits(void);
size_t allocator_next_fit_nodes_visited(void);
size_t allocator_next_fit_calls(void);

#endif