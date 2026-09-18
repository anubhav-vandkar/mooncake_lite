#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <cstddef>

struct mr_chunk {
    uint32_t slot_idx;
    void     *addr;
    uint32_t rkey;
    uint64_t vaddr;
    uint32_t size;
};

struct mr_pool {
    struct ibv_mr *mr;
    void          *base;
    size_t         slot_size;
    uint32_t       slot_count;
    uint8_t       *bitmap;      // 1 = free, 0 = allocated
};

mr_pool  *mr_pool_init(struct ibv_pd *pd, size_t slot_size, uint32_t slot_count);
mr_chunk *mr_pool_alloc(mr_pool *pool);
void      mr_pool_free(mr_pool *pool, mr_chunk *chunk);
void      mr_pool_destroy(mr_pool *pool);