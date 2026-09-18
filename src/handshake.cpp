#include "handshake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <infiniband/verbs.h>

int handshake_fill_local(struct ibv_context *ctx, struct ibv_qp *qp, qp_info *out) {
    out->qpn = qp->qp_num;
    out->psn = rand() & 0xFFFFFF;

    if (ibv_query_gid(ctx, 1, 2, &out->gid)) {
        fprintf(stderr, "ibv_query_gid failed\n");
        return -1;
    }

    fprintf(stderr, "local GID: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
        out->gid.raw[0],  out->gid.raw[1],
        out->gid.raw[2],  out->gid.raw[3],
        out->gid.raw[4],  out->gid.raw[5],
        out->gid.raw[6],  out->gid.raw[7],
        out->gid.raw[8],  out->gid.raw[9],
        out->gid.raw[10], out->gid.raw[11],
        out->gid.raw[12], out->gid.raw[13],
        out->gid.raw[14], out->gid.raw[15]);
    fflush(stderr);
    return 0;
}

static int qp_exchange(int sock, qp_info *local, qp_info *remote) {
    // send local, then recv remote
    ssize_t s = send(sock, local, sizeof(*local), 0);
    if (s != sizeof(*local)) {
        fprintf(stderr, "handshake send failed\n");
        return -1;
    }
    ssize_t r = recv(sock, remote, sizeof(*remote), MSG_WAITALL);
    if (r != sizeof(*remote)) {
        fprintf(stderr, "handshake recv failed\n");
        return -1;
    }
    return 0;
}

int handshake_server(int port, qp_info *local, qp_info *remote) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen"); close(listen_fd); return -1;
    }

    fprintf(stderr, "handshake: waiting for client on port %d\n", port);
    int conn_fd = accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
        perror("accept"); close(listen_fd); return -1;
    }

    int ret = qp_exchange(conn_fd, local, remote);

    fprintf(stderr, "[hs_server] local.qpn=%u remote.qpn=%u\n", local->qpn, remote->qpn);

    close(conn_fd);
    close(listen_fd);
    return ret;
}

int handshake_client(const char *host, int port, qp_info *local, qp_info *remote) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid address: %s\n", host);
        close(sock); return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sock); return -1;
    }

    int ret = qp_exchange(sock, local, remote);

    fprintf(stderr, "[hs_client] local.qpn=%u remote.qpn=%u\n", local->qpn, remote->qpn);
    close(sock);
    return ret;
}