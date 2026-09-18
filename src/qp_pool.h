#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <unordered_map>

struct qp_entry {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    uint32_t       remote_qpn;
    uint32_t       remote_psn;
    union ibv_gid  remote_gid;
    bool           connected;
};

struct qp_pool {
    struct ibv_pd                      *pd;
    struct ibv_context                 *ctx;
    std::unordered_map<uint32_t, qp_entry*> map;  // node_id -> qp_entry
};

qp_pool  *qp_pool_init(struct ibv_context *ctx, struct ibv_pd *pd);
qp_entry *qp_pool_get_or_create(qp_pool *pool, uint32_t node_id);
void      qp_pool_destroy(qp_pool *pool);