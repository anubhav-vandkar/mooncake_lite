#pragma once

#include <infiniband/verbs.h>

#include <cstdint>

struct qp_info {
    uint32_t      qpn;
    uint32_t      psn;
    union ibv_gid gid;
};

// fills remote with peer's qp_info
int handshake_server(int port, qp_info *local, qp_info *remote);
int handshake_client(const char *host, int port, qp_info *local, qp_info *remote);

// helper — fills local qp_info from context and qp
int handshake_fill_local(struct ibv_context *ctx, struct ibv_qp *qp, qp_info *out);