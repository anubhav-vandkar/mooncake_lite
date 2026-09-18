#include "mr_pool.h"

#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <sys/mman.h>

mr_pool *mr_pool_init(struct ibv_pd *pd, size_t slot_size, uint32_t slot_count) {
    fprintf(stderr, "mr_pool_init called\n"); fflush(stderr);
    
    size_t page = 4096;
    slot_size = (slot_size + page - 1) & ~(page - 1);
    size_t total = slot_size * slot_count;
    fprintf(stderr, "total size: %zu\n", total); fflush(stderr);

    void *buf = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fprintf(stderr, "mmap: %p\n", buf); fflush(stderr);
    if (buf == MAP_FAILED) return nullptr;

    struct ibv_mr *mr = ibv_reg_mr(pd, buf, total,
                                   IBV_ACCESS_LOCAL_WRITE  |
                                   IBV_ACCESS_REMOTE_WRITE |
                                   IBV_ACCESS_REMOTE_READ);
    fprintf(stderr, "ibv_reg_mr: %p errno: %d\n", (void*)mr, errno); fflush(stderr);
    if (!mr) {
        munmap(buf, total);
        return nullptr;
    }

    mr_pool *pool     = new mr_pool{};
    pool->mr          = mr;
    pool->base        = buf;
    pool->slot_size   = slot_size;
    pool->slot_count  = slot_count;
    pool->bitmap      = new uint8_t[slot_count];
    memset(pool->bitmap, 1, slot_count);  // all free

    std::fprintf(stderr, "mmap addr: %p\n", buf);
    std::fprintf(stderr, "ibv_reg_mr: %p\n", (void*)mr);

    return pool;
}

mr_chunk *mr_pool_alloc(mr_pool *pool) {
    for (uint32_t i = 0; i < pool->slot_count; i++) {
        if (pool->bitmap[i]) {
            pool->bitmap[i] = 0;
            mr_chunk *chunk = new mr_chunk{};
            fprintf(stderr, "chunk ptr: %p\n", (void*)chunk);
            chunk->slot_idx = i;
            fprintf(stderr, "addr calc: base=%p i=%u slot_size=%zu\n", pool->base, i, pool->slot_size);
            chunk->addr = (uint8_t *)pool->base + i * pool->slot_size;
            fprintf(stderr, "chunk->addr: %p\n", chunk->addr);
            chunk->rkey  = pool->mr->rkey;
            fprintf(stderr, "rkey: %u\n", chunk->rkey);
            chunk->vaddr    = (uint64_t)chunk->addr;
            chunk->size     = (uint32_t)pool->slot_size;
            return chunk;
        }
    }
    return nullptr;  // pool exhausted
}

void mr_pool_free(mr_pool *pool, mr_chunk *chunk) {
    pool->bitmap[chunk->slot_idx] = 1;
    delete chunk;
}

void mr_pool_destroy(mr_pool *pool) {
    ibv_dereg_mr(pool->mr);
    munmap(pool->base, pool->slot_size * pool->slot_count);
    delete[] pool->bitmap;
    delete pool;
}