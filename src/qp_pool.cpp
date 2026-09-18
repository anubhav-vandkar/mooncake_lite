#include "qp_pool.h"
#include <cstring>
#include <cstdio>

static qp_entry *create_qp(struct ibv_context *ctx, struct ibv_pd *pd) {
    struct ibv_cq *cq = ibv_create_cq(ctx, 64, nullptr, nullptr, 0);
    if (!cq) return nullptr;

    struct ibv_qp_init_attr attr{};
    attr.send_cq          = cq;
    attr.recv_cq          = cq;
    attr.qp_type          = IBV_QPT_RC;
    attr.cap.max_send_wr  = 64;
    attr.cap.max_recv_wr  = 64;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    struct ibv_qp *qp = ibv_create_qp(pd, &attr);
    if (!qp) {
        ibv_destroy_cq(cq);
        return nullptr;
    }

    // move to INIT
    struct ibv_qp_attr qp_attr{};
    qp_attr.qp_state        = IBV_QPS_INIT;
    qp_attr.pkey_index      = 0;
    qp_attr.port_num        = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                              IBV_ACCESS_REMOTE_READ  |
                              IBV_ACCESS_LOCAL_WRITE;

    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS)) {
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        return nullptr;
    }

    qp_entry *e   = new qp_entry{};
    e->qp         = qp;
    e->cq         = cq;
    e->connected  = false;
    return e;
}

qp_pool *qp_pool_init(struct ibv_context *ctx, struct ibv_pd *pd) {
    qp_pool *pool = new qp_pool{};
    pool->ctx     = ctx;
    pool->pd      = pd;
    return pool;
}

qp_entry *qp_pool_get_or_create(qp_pool *pool, uint32_t node_id) {
    auto it = pool->map.find(node_id);
    if (it != pool->map.end()) return it->second;

    qp_entry *e = create_qp(pool->ctx, pool->pd);
    if (!e) return nullptr;
    pool->map[node_id] = e;
    return e;
}

void qp_pool_destroy(qp_pool *pool) {
    for (auto &[id, e] : pool->map) {
        ibv_destroy_qp(e->qp);
        ibv_destroy_cq(e->cq);
        delete e;
    }
    delete pool;
}