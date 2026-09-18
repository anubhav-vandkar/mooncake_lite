#include "src/transfer.h"
#include "src/handshake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#define PORT      7777
#define HOST      "127.0.0.1"
#define DEV       "rxe0"
#define SLOT_SIZE (512 * 1024)   // 512KB — fits under memlock limit
#define SLOT_CNT  8

static void run_server(transfer_ctx *tctx) {
    // get a QP to handshake with
    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    if (!e) { fprintf(stderr, "qp_pool_get_or_create failed\n"); return; }

    qp_info local{}, remote{};
    if (handshake_fill_local(tctx->ctx, e->qp, &local)) return;
    fprintf(stderr, "[server] local  qpn=%u psn=%u\n", local.qpn, local.psn);

    if (handshake_server(PORT, &local, &remote)) return;
    fprintf(stderr, "[server] remote qpn=%u psn=%u\n", remote.qpn, remote.psn);

    if (transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid)) {
        fprintf(stderr, "transfer_connect failed\n"); return;
    }
    fprintf(stderr, "[server] QP connected\n");

    // allocate a chunk and expose it to the client
    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    if (!chunk) { fprintf(stderr, "mr_pool_alloc failed\n"); return; }

    // write a known pattern so client can verify
    memset(chunk->addr, 0xAB, chunk->size);
    fprintf(stderr, "[server] chunk ready: vaddr=0x%lx rkey=0x%x size=%u\n",
            chunk->vaddr, chunk->rkey, chunk->size);

    // send chunk metadata to client over a second TCP connection
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT + 1);
    bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(lfd, 1);
    int cfd = accept(lfd, nullptr, nullptr);

    struct { uint64_t vaddr; uint32_t rkey; uint32_t size; } meta{};
    meta.vaddr = chunk->vaddr;
    meta.rkey  = chunk->rkey;
    meta.size  = chunk->size;
    send(cfd, &meta, sizeof(meta), 0);

    // wait for client to signal done
    char done = 0;
    recv(cfd, &done, 1, 0);
    fprintf(stderr, "[server] client signaled done\n");

    close(cfd);
    close(lfd);
    mr_pool_free(tctx->mrp, chunk);
}

static void run_client(transfer_ctx *tctx) {
    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    if (!e) { fprintf(stderr, "qp_pool_get_or_create failed\n"); return; }

    qp_info local{}, remote{};
    if (handshake_fill_local(tctx->ctx, e->qp, &local)) return;
    fprintf(stderr, "[client] local  qpn=%u psn=%u\n", local.qpn, local.psn);

    if (handshake_client(HOST, PORT, &local, &remote)) return;
    fprintf(stderr, "[client] remote qpn=%u psn=%u\n", remote.qpn, remote.psn);

    if (transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid)) {
        fprintf(stderr, "transfer_connect failed\n"); return;
    }
    fprintf(stderr, "[client] QP connected\n");

    // get chunk metadata from server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT + 1);
    inet_pton(AF_INET, HOST, &addr.sin_addr);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    struct { uint64_t vaddr; uint32_t rkey; uint32_t size; } meta{};
    recv(sock, &meta, sizeof(meta), MSG_WAITALL);
    fprintf(stderr, "[client] server chunk: vaddr=0x%lx rkey=0x%x size=%u\n",
            meta.vaddr, meta.rkey, meta.size);

    // allocate local chunk for RDMA-READ target
    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    if (!chunk) { fprintf(stderr, "mr_pool_alloc failed\n"); return; }
    memset(chunk->addr, 0, chunk->size);

    // issue RDMA-READ from server's chunk into our local chunk
    if (transfer_read(tctx, 0, chunk, meta.vaddr, meta.rkey)) {
        fprintf(stderr, "transfer_read failed\n"); return;
    }
    if (transfer_poll(tctx, 0, 1)) {
        fprintf(stderr, "transfer_poll failed\n"); return;
    }

    // verify pattern
    uint8_t *buf = (uint8_t *)chunk->addr;
    bool ok = true;
    for (uint32_t i = 0; i < chunk->size; i++) {
        if (buf[i] != 0xAB) { ok = false; break; }
    }
    fprintf(stderr, "[client] data verify: %s\n", ok ? "PASS" : "FAIL");

    // signal done
    char done = 1;
    send(sock, &done, 1, 0);
    close(sock);
    mr_pool_free(tctx->mrp, chunk);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1], "--server") && strcmp(argv[1], "--client"))) {
        fprintf(stderr, "usage: %s --server | --client\n", argv[0]);
        return 1;
    }

    bool is_server = strcmp(argv[1], "--server") == 0;

    transfer_ctx *tctx = transfer_init(DEV, SLOT_SIZE, SLOT_CNT);
    if (!tctx) { fprintf(stderr, "transfer_init failed\n"); return 1; }

    if (is_server) run_server(tctx);
    else           run_client(tctx);

    transfer_destroy(tctx);
    return 0;
}