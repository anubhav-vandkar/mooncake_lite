#pragma once

#include "mr_pool.h"
#include "qp_pool.h"

#include <cstdint>

struct transfer_ctx {
    struct ibv_context *ctx;
    struct ibv_pd      *pd;
    mr_pool            *mrp;
    qp_pool            *qpp;
};

transfer_ctx *transfer_init(const char *dev_name, size_t slot_size, uint32_t slot_count);

void transfer_destroy(transfer_ctx *tctx);

// connect local QP to remote (exchange via out-of-band TCP)
int transfer_connect(transfer_ctx *tctx, uint32_t node_id,
                     uint32_t remote_qpn, uint32_t remote_psn,
                     union ibv_gid remote_gid);

// RDMA-WRITE: push local chunk to remote addr
int transfer_write(transfer_ctx *tctx, uint32_t node_id,
                   mr_chunk *local_chunk,
                   uint64_t remote_vaddr, uint32_t remote_rkey);

// RDMA-READ: pull remote addr into local chunk
int transfer_read(transfer_ctx *tctx, uint32_t node_id,
                  mr_chunk *local_chunk,
                  uint64_t remote_vaddr, uint32_t remote_rkey);

// poll CQ until n completions or error
int transfer_poll(transfer_ctx *tctx, uint32_t node_id, int n);