#include<stddef.h>
#include<stdlib.h>
#include<stdio.h>
#include<stdint.h>
#include<limits.h>
#include<sys/mman.h>
#include<unistd.h>
#include "allocator.h"

#define ALLIGNMENT 16
#define ALLIGN_UP(n) (((n) + (ALLIGNMENT - 1)) & ~(ALLIGNMENT - 1))

typedef struct Header{
    size_t size;
    uint8_t free;
}header_t;

typedef struct Footer{
    size_t size;
}footer_t;

static char *mem_start = NULL;
static size_t mem_size = 0;
static char *bump_ptr = NULL;
static char *last_found_slot = NULL;
static uint8_t curr_policy;
static size_t next_fit_pass1_hits = 0;
static size_t next_fit_pass2_hits = 0;
static size_t next_fit_nodes_visited = 0;
static size_t next_fit_calls = 0;
static const size_t HEADER_SIZE = (ALLIGN_UP(sizeof(header_t)));
static const size_t FOOTER_SIZE = (ALLIGN_UP(sizeof(footer_t)));
static const size_t THRESHOLD = HEADER_SIZE + ALLIGNMENT + FOOTER_SIZE;

void allocator_init(size_t init_size, uint8_t pol) {
    long page = sysconf(_SC_PAGESIZE);
    init_size = ((init_size + page - 1) / page) * page;
    mem_start = (char *)mmap(NULL, init_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(mem_start == MAP_FAILED) {
        printf("mmap failed to allocate space");
        mem_start = NULL;
        return;
    }
    last_found_slot = mem_start;
    mem_size = init_size;
    bump_ptr = mem_start;
    curr_policy = pol;
    next_fit_pass1_hits = 0;
    next_fit_pass2_hits = 0;
    next_fit_nodes_visited = 0;
    next_fit_calls = 0;
}

void allocator_destroy(void) {
    if(mem_start) munmap(mem_start, mem_size);
    mem_start = NULL;
    bump_ptr = NULL;
    mem_size = 0;
    last_found_slot = NULL;
}

header_t* findFirstFit(size_t size) {
    char *curr_ptr = mem_start;
    while(curr_ptr < bump_ptr) {
        header_t *curr_header = (header_t *)curr_ptr;
        if(curr_header->free == 1 && curr_header->size >= size) return curr_header;
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
return NULL;
}

header_t* findBestFit(size_t size) {
    char *curr_ptr = mem_start; header_t *best_slot = NULL;
    size_t best = INT_MAX;
    while(curr_ptr < bump_ptr) {
        header_t* curr_header = (header_t*)curr_ptr;
        if(curr_header->free == 1 && curr_header->size >= size) {
            size_t diff = curr_header->size - size;
            if(best > diff) {
                best = diff;
                best_slot = curr_header;
            }
        }
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return best_slot;
}

header_t* findWorstFit(size_t size) {
    char *curr_ptr = mem_start; header_t *worst_slot = NULL;
    size_t worst = 0;
    while(curr_ptr < bump_ptr) {
        header_t* curr_header = (header_t*)curr_ptr;
        if(curr_header->free == 1 && curr_header->size >= size) {
            size_t diff = curr_header->size - size;
            if(worst <= diff) {
                worst = diff;
                worst_slot = curr_header;
            }
        }
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return worst_slot;
}

header_t* findNextFit(size_t size) {
    next_fit_calls++;
    char *curr_ptr = last_found_slot;
    while(curr_ptr < bump_ptr) {
        header_t *curr_header = (header_t *)curr_ptr;
        next_fit_nodes_visited++;
        if(curr_header->free == 1 && curr_header->size >= size) {
            last_found_slot = (char*)curr_header;
            next_fit_pass1_hits++;
            return curr_header;
        }
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    curr_ptr = mem_start;
    while(curr_ptr < last_found_slot) {
        header_t *curr_header = (header_t *)curr_ptr;
        next_fit_nodes_visited++;
        if(curr_header->free == 1 && curr_header->size >= size) {
            last_found_slot = (char*)curr_header;
            next_fit_pass2_hits++;
            return curr_header;
        }
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return NULL;
}

header_t* findFreeSlot(size_t size) {
    switch(curr_policy) {
        case FIRST_FIT:
            return findFirstFit(size);
        case BEST_FIT:
            return findBestFit(size);
        case NEXT_FIT:
            return findNextFit(size);
        case WORST_FIT:
            return findWorstFit(size);
        default:
            return findFirstFit(size);
    }
}

void split_block(header_t* block, size_t size) {
    header_t* new_header = (header_t*)((char*)block + HEADER_SIZE + size + FOOTER_SIZE);
    new_header->free = 1;
    size_t new_block_space = block->size - size - HEADER_SIZE - FOOTER_SIZE;
    new_header->size = new_block_space;
    footer_t* new_footer = (footer_t*)((char*)new_header + HEADER_SIZE + new_block_space);
    new_footer->size = new_block_space;
    block->size = size;
}

void *custom_malloc(size_t size) {
    if(size == 0) return NULL;
    size_t alligned_size = ALLIGN_UP(size);
    size_t allocated_size = alligned_size + HEADER_SIZE + FOOTER_SIZE;

    header_t *new_header = findFreeSlot(alligned_size);
    footer_t* new_footer;
    if(new_header) {
        if(new_header->size - alligned_size >= THRESHOLD) split_block(new_header, alligned_size);
        new_header->free = 0;
        new_footer = (footer_t*)((char*)new_header + new_header->size + HEADER_SIZE);
        new_footer->size = new_header->size;
        return (char *)new_header + HEADER_SIZE;
    }

    if(bump_ptr + allocated_size > mem_start + mem_size) return NULL;

    char *curr_ptr = bump_ptr;
    new_header = (header_t *)curr_ptr;
    new_header->size = alligned_size;
    new_header->free = 0;
    new_footer = (footer_t*)(curr_ptr + alligned_size + HEADER_SIZE);
    new_footer->size = alligned_size;

    curr_ptr += HEADER_SIZE;
    bump_ptr += allocated_size;
    return curr_ptr;
}

void joinL(header_t* left, header_t* curr) {
    if(last_found_slot == (char*)curr) last_found_slot = (char*)left;
    left->size = left->size + curr->size + (HEADER_SIZE + FOOTER_SIZE);
    footer_t* footer = (footer_t*)((char*)left + left->size + HEADER_SIZE);
    footer->size = left->size;
    if((char *)left != mem_start) {
        footer_t* prev_f = (footer_t*)((char*)left - FOOTER_SIZE);
        header_t* prev_h = (header_t*)((char*)prev_f - prev_f->size - HEADER_SIZE);
        if(prev_h->free == 1) joinL(prev_h, left);
    }
}
void joinR(header_t* right, header_t* curr) {
    if(last_found_slot == (char*)right) last_found_slot = (char*)curr;
    curr->size = curr->size + right->size + (HEADER_SIZE + FOOTER_SIZE);
    footer_t* footer = (footer_t*)((char*)curr + curr->size + HEADER_SIZE);
    footer->size = curr->size;
    header_t* next_header = (header_t*)((char*)curr + HEADER_SIZE + FOOTER_SIZE + curr->size);
    if((char *)next_header < bump_ptr && next_header->free == 1) joinR(next_header, curr);
}

size_t allocator_total_free(void) {
    size_t total = 0;
    char *curr_ptr = mem_start;
    while(curr_ptr < bump_ptr) {
        header_t *curr_header = (header_t*)curr_ptr;
        if(curr_header->free == 1) total += curr_header->size;
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return total;
}

size_t allocator_largest_free(void) {
    size_t largest = 0;
    char *curr_ptr = mem_start;
    while(curr_ptr < bump_ptr) {
        header_t *curr_header = (header_t*)curr_ptr;
        if(curr_header->free == 1 && curr_header->size > largest) largest = curr_header->size;
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return largest;
}

size_t allocator_free_block_count(void) {
    size_t count = 0;
    char *curr_ptr = mem_start;
    while(curr_ptr < bump_ptr) {
    header_t *curr_header = (header_t*)curr_ptr;
    if(curr_header->free == 1) count++;
        curr_ptr += HEADER_SIZE + curr_header->size + FOOTER_SIZE;
    }
    return count;
}

size_t allocator_next_fit_pass1_hits(void) { return next_fit_pass1_hits; }
size_t allocator_next_fit_pass2_hits(void) { return next_fit_pass2_hits; }
size_t allocator_next_fit_nodes_visited(void) { return next_fit_nodes_visited; }
size_t allocator_next_fit_calls(void) { return next_fit_calls; }

void custom_free(void *ptr) {
    if(!ptr) return;
    header_t *curr_header = (header_t *)((char *)ptr - HEADER_SIZE);
    if(curr_header->free == 1) {
        printf("warning: double free detected\n");
        return;
    }
    curr_header->free = 1;
    header_t *right_h = (header_t*)((char*)curr_header + curr_header->size + HEADER_SIZE + FOOTER_SIZE);
    if((char *)right_h < bump_ptr && right_h->free == 1) joinR(right_h, curr_header);
    if((char *)curr_header != mem_start) {
        footer_t* left_f = (footer_t*)((char*)curr_header - FOOTER_SIZE);
        header_t* left_h = (header_t*)((char*)left_f - left_f->size - HEADER_SIZE);
        if(left_h->free == 1) joinL(left_h, curr_header);
    }
}