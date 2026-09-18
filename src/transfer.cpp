#include "transfer.h"
#include <string>
#include <cstdio>

#include <infiniband/verbs.h>

transfer_ctx *transfer_init(const char *dev_name, size_t slot_size, uint32_t slot_count) {
    int num_devs = 0;
    struct ibv_device **devs = ibv_get_device_list(&num_devs);
    fprintf(stderr, "num_devs: %d\n", num_devs); fflush(stderr);
    if (!devs || num_devs == 0) 
        return nullptr;

    struct ibv_device *dev = nullptr;
    for (int i = 0; i < num_devs; i++) {
        fprintf(stderr, "found device: %s\n", ibv_get_device_name(devs[i]));
        fflush(stderr);
        if (std::string(ibv_get_device_name(devs[i])) == dev_name) {
            dev = devs[i];
            break;
        }
    }
    if (!dev) { 
        fprintf(stderr, "device %s not found\n", dev_name);
        ibv_free_device_list(devs); 
        return nullptr; 
    }

    struct ibv_context *ctx = ibv_open_device(dev);
    ibv_free_device_list(devs);
    if (!ctx) { fprintf(stderr, "ibv_open_device failed\n"); return nullptr; }
    fprintf(stderr, "ctx ok\n");

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { fprintf(stderr, "ibv_alloc_pd failed\n"); return nullptr; }
    fprintf(stderr, "pd ok\n");

    transfer_ctx *tctx = new transfer_ctx{};
    tctx->ctx = ctx;
    tctx->pd  = pd;
    tctx->mrp = mr_pool_init(pd, slot_size, slot_count);
    tctx->qpp = qp_pool_init(ctx, pd);
    return tctx;
}

void transfer_destroy(transfer_ctx *tctx) {
    qp_pool_destroy(tctx->qpp);
    mr_pool_destroy(tctx->mrp);
    ibv_dealloc_pd(tctx->pd);
    ibv_close_device(tctx->ctx);
    delete tctx;
}

int transfer_connect(transfer_ctx *tctx, uint32_t node_id, uint32_t remote_qpn, uint32_t remote_psn, union ibv_gid remote_gid) {
    qp_entry *e = qp_pool_get_or_create(tctx->qpp, node_id);
    if (!e) return -1;

     struct ibv_port_attr port_attr{};
    ibv_query_port(tctx->ctx, 1, &port_attr);

    struct ibv_qp_attr attr{};
    attr.qp_state                  = IBV_QPS_RTR;
    attr.path_mtu                  = port_attr.active_mtu;
    attr.dest_qp_num               = remote_qpn;
    attr.rq_psn                    = remote_psn;
    attr.max_dest_rd_atomic        = 0;
    attr.min_rnr_timer             = 12;
    attr.ah_attr.dlid              = port_attr.lid;
    attr.ah_attr.sl                = 0;
    attr.ah_attr.src_path_bits     = 0;
    attr.ah_attr.port_num          = 1;
    attr.ah_attr.is_global         = 1;
    attr.ah_attr.grh.dgid          = remote_gid;
    attr.ah_attr.grh.sgid_index    = 2;
    attr.ah_attr.grh.hop_limit     = 64;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.grh.flow_label    = 0;

    errno = 0;
    int rtr_ret = ibv_modify_qp(e->qp, &attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                    IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                    IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    fprintf(stderr, "RTR ibv_modify_qp ret=%d errno=%d (%s)\n", 
        rtr_ret, errno, strerror(errno));
    fflush(stderr);
    if (rtr_ret || errno) {
        fprintf(stderr, "RTR failed: ret=%d errno=%d (%s)\n",
                rtr_ret, errno, strerror(errno));
        return -1;
    }

    fprintf(stderr, "RTR: dest_qpn=%u rq_psn=%u sgid_index=%d\n",
        attr.dest_qp_num, attr.rq_psn, attr.ah_attr.grh.sgid_index);
    fprintf(stderr, "RTR: dgid=%02x%02x...%02x%02x\n",
            attr.ah_attr.grh.dgid.raw[0],
            attr.ah_attr.grh.dgid.raw[1],
            attr.ah_attr.grh.dgid.raw[14],
            attr.ah_attr.grh.dgid.raw[15]);
    fflush(stderr);
    

    struct ibv_qp_attr query_attr{};
    struct ibv_qp_init_attr query_init{};
    ibv_query_qp(e->qp, &query_attr, IBV_QP_STATE, &query_init);
    fprintf(stderr, "QP state after RTR attempt: %d (RTR=4)\n", query_attr.qp_state);
    fflush(stderr);

    // RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;

    errno = 0;
    int rts_ret = ibv_modify_qp(e->qp, &attr,
                    IBV_QP_STATE      |
                    IBV_QP_SQ_PSN     |
                    IBV_QP_MAX_QP_RD_ATOMIC);
    if (rts_ret || errno) {
        fprintf(stderr, "RTS failed: ret=%d errno=%d (%s)\n",
                rts_ret, errno, strerror(errno));
        return -1;
    }

    e->connected = true;
    return 0;
}

static int post_rdma(transfer_ctx *tctx, uint32_t node_id,
                     mr_chunk *local_chunk,
                     uint64_t remote_vaddr, uint32_t remote_rkey,
                     enum ibv_wr_opcode opcode) {
    qp_entry *e = tctx->qpp->map.at(node_id);

    struct ibv_sge sge{};
    sge.addr   = (uint64_t)local_chunk->addr;
    sge.length = local_chunk->size;
    sge.lkey   = tctx->mrp->mr->lkey;

    struct ibv_send_wr wr{};
    wr.wr_id                = (uint64_t)local_chunk->slot_idx;
    wr.sg_list              = &sge;
    wr.num_sge              = 1;
    wr.opcode               = opcode;
    wr.send_flags           = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr  = remote_vaddr;
    wr.wr.rdma.rkey         = remote_rkey;

    struct ibv_send_wr *bad_wr = nullptr;
    int ret = ibv_post_send(e->qp, &wr, &bad_wr);

    // QP state check
    struct ibv_qp_attr attr{};
    struct ibv_qp_init_attr init_attr{};
    ibv_query_qp(e->qp, &attr, IBV_QP_STATE, &init_attr);
    fprintf(stderr, "QP state after post_send: %d (RTS=5)\n", attr.qp_state);
    fflush(stderr);

    return ret;
}

int transfer_write(transfer_ctx *tctx, uint32_t node_id,
                   mr_chunk *local_chunk,
                   uint64_t remote_vaddr, uint32_t remote_rkey) {
    return post_rdma(tctx, node_id, local_chunk,
                     remote_vaddr, remote_rkey, IBV_WR_RDMA_WRITE);
}

int transfer_read(transfer_ctx *tctx, uint32_t node_id,
                  mr_chunk *local_chunk,
                  uint64_t remote_vaddr, uint32_t remote_rkey) {
    return post_rdma(tctx, node_id, local_chunk,
                     remote_vaddr, remote_rkey, IBV_WR_RDMA_READ);
}

int transfer_poll(transfer_ctx *tctx, uint32_t node_id, int n) {
    qp_entry *e = tctx->qpp->map.at(node_id);
    struct ibv_wc wc[16];
    int done = 0;
    int attempts = 0;
    while (done < n) {
        int ret = ibv_poll_cq(e->cq, 16, wc);
        if (ret < 0) return -1;
        if (ret == 0) {
            attempts++;
            if (attempts % 1000000 == 0) {
                fprintf(stderr, "poll: still waiting... attempts=%d\n", attempts);
                fflush(stderr);
            }
            if (attempts > 10000000) {
                fprintf(stderr, "transfer_poll: timeout\n");
                return -1;
            }
            continue;
        }
        for (int i = 0; i < ret; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "CQ error: %s\n", ibv_wc_status_str(wc[i].status));
                return -1;
            }
            done++;
        }
        attempts = 0;
    }
    return 0;
}