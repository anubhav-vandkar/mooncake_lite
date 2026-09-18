#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <algorithm>
#include <vector>
#include <string>

#include <infiniband/verbs.h>

#include "../src/transfer.h"
#include "../src/handshake.h"

//  Timing
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

//  Config
#define DEV          "rxe0"
#define GID_IDX      1
#define BASE_PORT    8000
#define ITERATIONS   1000
#define WARMUP       50

static const size_t SIZES[] = {
    4   * 1024,
    16  * 1024,
    64  * 1024,
    256 * 1024,
    512 * 1024
};
static const int NSIZES = sizeof(SIZES) / sizeof(SIZES[0]);

//  Helpers
static uint64_t percentile(std::vector<uint64_t> &v, double p) {
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(p / 100.0 * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

// simple barrier over a socket pair
static void barrier(int sock) {
    char b = 1;
    send(sock, &b, 1, 0);
    recv(sock, &b, 1, MSG_WAITALL);
}

// make a socketpair for parent<->child sync
static void make_sync_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair"); exit(1);
    }
}

//  CSV / gnuplot
static FILE *csv_open(const char *name) {
    FILE *f = fopen(name, "w");
    if (!f) { perror("fopen"); exit(1); }
    return f;
}

static void gnuplot_lat(const char *csv) {
    FILE *gp = fopen("lat.gp", "w");
    fprintf(gp,
        "set terminal png size 900,600\n"
        "set output 'lat.png'\n"
        "set title 'RDMA-READ Latency vs Transfer Size'\n"
        "set xlabel 'Transfer Size (KB)'\n"
        "set ylabel 'Latency (us)'\n"
        "set logscale x 2\n"
        "set grid\n"
        "set key top left\n"
        "plot '%s' using ($1/1024):2 with linespoints title 'p50',\\\n"
        "     '%s' using ($1/1024):3 with linespoints title 'p99',\\\n"
        "     '%s' using ($1/1024):4 with linespoints title 'p999'\n",
        csv, csv, csv);
    fclose(gp);
    system("gnuplot lat.gp");
    printf("wrote lat.png\n");
}

static void gnuplot_tput(const char *csv) {
    FILE *gp = fopen("tput.gp", "w");
    fprintf(gp,
        "set terminal png size 900,600\n"
        "set output 'tput.png'\n"
        "set title 'RDMA-WRITE Throughput vs Transfer Size'\n"
        "set xlabel 'Transfer Size (KB)'\n"
        "set ylabel 'Throughput (GB/s)'\n"
        "set logscale x 2\n"
        "set grid\n"
        "set key top left\n"
        "plot '%s' using ($1/1024):2 with linespoints title 'throughput'\n",
        csv);
    fclose(gp);
    system("gnuplot tput.gp");
    printf("wrote tput.png\n");
}

static void gnuplot_mr(const char *csv) {
    FILE *gp = fopen("mr.gp", "w");
    fprintf(gp,
        "set terminal png size 900,600\n"
        "set output 'mr.png'\n"
        "set title 'MR Pool vs Per-transfer Registration Cost'\n"
        "set xlabel 'Transfer Size (KB)'\n"
        "set ylabel 'Registration Cost (us)'\n"
        "set logscale x 2\n"
        "set grid\n"
        "set key top left\n"
        "plot '%s' using ($1/1024):2 with linespoints title 'pool alloc',\\\n"
        "     '%s' using ($1/1024):3 with linespoints title 'ibv_reg_mr'\n",
        csv, csv);
    fclose(gp);
    system("gnuplot mr.gp");
    printf("wrote mr.png\n");
}

static void gnuplot_qp(const char *csv) {
    FILE *gp = fopen("qp.gp", "w");
    fprintf(gp,
        "set terminal png size 900,600\n"
        "set output 'qp.png'\n"
        "set title 'QP Reuse vs Per-transfer QP Setup Cost'\n"
        "set xlabel 'Iteration'\n"
        "set ylabel 'Setup Cost (us)'\n"
        "set grid\n"
        "set key top left\n"
        "plot '%s' using 1:2 with linespoints title 'reused QP',\\\n"
        "     '%s' using 1:3 with linespoints title 'new QP'\n",
        csv, csv);
    fclose(gp);
    system("gnuplot qp.gp");
    printf("wrote qp.png\n");
}

//  Benchmark 1: Latency
struct chunk_meta { uint64_t vaddr; uint32_t rkey; uint32_t size; };

static void lat_server(int sync) {
    transfer_ctx *tctx = transfer_init(DEV, 512*1024, 8);
    assert(tctx);

    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    assert(e);

    // open meta listen socket BEFORE handshake
    int msock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(BASE_PORT + 1);
    bind(msock, (struct sockaddr*)&a, sizeof(a));
    listen(msock, 1);

    // now do handshake
    qp_info local{}, remote{};
    handshake_fill_local(tctx->ctx, e->qp, &local);
    handshake_server(BASE_PORT, &local, &remote);
    //transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);
    fprintf(stderr, "[lat_server] calling transfer_connect\n"); fflush(stderr);
    int rc = transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);
    fprintf(stderr, "[lat_server] transfer_connect ret=%d\n", rc); fflush(stderr);
    fprintf(stderr, "transfer_connect ret=%d errno=%d (%s)\n", rc, errno, strerror(errno));
    fflush(stderr);

    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    assert(chunk);
    memset(chunk->addr, 0xAB, chunk->size);

    // now accept meta connection
    int mc = accept(msock, nullptr, nullptr);
    chunk_meta meta{ chunk->vaddr, chunk->rkey, chunk->size };
    send(mc, &meta, sizeof(meta), 0);

    for (int s = 0; s < NSIZES; s++) {
        // wait for client to finish this size
        char b; recv(mc, &b, 1, MSG_WAITALL);
        send(mc, &b, 1, 0);
    }
    close(mc); close(msock);

    // signal parent
    char done = 1; write(sync, &done, 1);
    mr_pool_free(tctx->mrp, chunk);
    transfer_destroy(tctx);
}

static void lat_client(int sync, FILE *csv) {
    transfer_ctx *tctx = transfer_init(DEV, 512*1024, 8);
    assert(tctx);

    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    assert(e);

    qp_info local{}, remote{};
    handshake_fill_local(tctx->ctx, e->qp, &local);
    fprintf(stderr, "[lat_client] local qpn=%u\n", local.qpn);

    // handshake FIRST
    usleep(500000);
    handshake_client("127.0.0.1", BASE_PORT, &local, &remote);
    fprintf(stderr, "[lat_client] handshake done\n");
    //transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);
    int rc = transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);
    fprintf(stderr, "transfer_connect ret=%d errno=%d (%s)\n", rc, errno, strerror(errno));
    fflush(stderr);
    fprintf(stderr, "[lat_client] QP connected\n");

    // THEN meta socket
    int msock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(BASE_PORT + 1);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    usleep(200000);
    connect(msock, (struct sockaddr*)&a, sizeof(a));

    chunk_meta meta{};
    recv(msock, &meta, sizeof(meta), MSG_WAITALL);
    fprintf(stderr, "[lat_client] got meta vaddr=0x%lx rkey=0x%x\n",
            meta.vaddr, meta.rkey);

    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    assert(chunk);

    fprintf(csv, "# size_bytes p50_us p99_us p999_us\n");

    for (int s = 0; s < NSIZES; s++) {
        size_t sz = SIZES[s];
        std::vector<uint64_t> samples;
        samples.reserve(ITERATIONS);

        for (int i = 0; i < WARMUP; i++) {
            //transfer_read(tctx, 0, chunk, meta.vaddr, meta.rkey);
            //transfer_poll(tctx, 0, 1);

            fprintf(stderr, "[lat_client] posting read %d\n", i); fflush(stderr);
            int r = transfer_read(tctx, 0, chunk, meta.vaddr, meta.rkey);
            fprintf(stderr, "[lat_client] post_send ret=%d\n", r); fflush(stderr);
            int p = transfer_poll(tctx, 0, 1);
            fprintf(stderr, "[lat_client] poll ret=%d\n", p); fflush(stderr);
        }

        for (int i = 0; i < ITERATIONS; i++) {
            uint64_t t0 = now_ns();
            transfer_read(tctx, 0, chunk, meta.vaddr, meta.rkey);
            transfer_poll(tctx, 0, 1);
            uint64_t t1 = now_ns();
            samples.push_back(t1 - t0);
        }

        uint64_t p50  = percentile(samples, 50.0)  / 1000;
        uint64_t p99  = percentile(samples, 99.0)  / 1000;
        uint64_t p999 = percentile(samples, 99.9)  / 1000;

        fprintf(csv, "%zu %lu %lu %lu\n", sz, p50, p99, p999);
        fflush(csv);
        printf("[lat] size=%5zuKB  p50=%4luus  p99=%4luus  p999=%4luus\n",
               sz/1024, p50, p99, p999);

        char b = 1; send(msock, &b, 1, 0);
        recv(msock, &b, 1, MSG_WAITALL);
    }

    close(msock);
    char done = 1; write(sync, &done, 1);
    mr_pool_free(tctx->mrp, chunk);
    transfer_destroy(tctx);
}

static void bench_latency() {
    printf("\n=== Latency Benchmark ===\n");

    int sv[2], cv[2];
    make_sync_pair(sv); make_sync_pair(cv);

    // spawn server child as fresh process
    pid_t spid = fork();
    if (spid == 0) {
        close(sv[0]);
        char fd_str[16]; snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        execl("/proc/self/exe", "mooncake_bench",
              "--child-lat-server", fd_str, nullptr);
        perror("execl"); exit(1);
    }

    usleep(300000);

    pid_t cpid = fork();
    if (cpid == 0) {
        close(cv[0]);
        char fd_str[16]; snprintf(fd_str, sizeof(fd_str), "%d", cv[1]);
        execl("/proc/self/exe", "mooncake_bench",
              "--child-lat-client", fd_str, nullptr);
        perror("execl"); exit(1);
    }

    char b;
    read(sv[0], &b, 1);
    read(cv[0], &b, 1);
    waitpid(spid, nullptr, 0);
    waitpid(cpid, nullptr, 0);
    gnuplot_lat("lat.csv");
}

// ------------------------------------------------------------------ //
//  Benchmark 2: Throughput
// ------------------------------------------------------------------ //
static void tput_server(int sync) {
    transfer_ctx *tctx = transfer_init(DEV, 512*1024, 8);
    assert(tctx);

    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    assert(e);

    int msock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(BASE_PORT + 3);
    bind(msock, (struct sockaddr*)&a, sizeof(a));
    listen(msock, 1);

    qp_info local{}, remote{};
    handshake_fill_local(tctx->ctx, e->qp, &local);
    handshake_server(BASE_PORT + 2, &local, &remote);
    transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);

    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    assert(chunk);

    int mc = accept(msock, nullptr, nullptr);
    chunk_meta meta{ chunk->vaddr, chunk->rkey, chunk->size };
    send(mc, &meta, sizeof(meta), 0);

    for (int s = 0; s < NSIZES; s++) {
        char b; recv(mc, &b, 1, MSG_WAITALL);
        send(mc, &b, 1, 0);
    }

    close(mc); close(msock);
    char done = 1; write(sync, &done, 1);
    mr_pool_free(tctx->mrp, chunk);
    transfer_destroy(tctx);
}

static void tput_client(int sync, FILE *csv) {
    transfer_ctx *tctx = transfer_init(DEV, 512*1024, 8);
    assert(tctx);

    qp_entry *e = qp_pool_get_or_create(tctx->qpp, 0);
    assert(e);

    qp_info local{}, remote{};
    handshake_fill_local(tctx->ctx, e->qp, &local);

    usleep(500000);
    handshake_client("127.0.0.1", BASE_PORT + 2, &local, &remote);
    transfer_connect(tctx, 0, remote.qpn, remote.psn, remote.gid);

    int msock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(BASE_PORT + 3);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    usleep(200000);
    connect(msock, (struct sockaddr*)&a, sizeof(a));

    chunk_meta meta{};
    recv(msock, &meta, sizeof(meta), MSG_WAITALL);

    mr_chunk *chunk = mr_pool_alloc(tctx->mrp);
    assert(chunk);

    fprintf(csv, "# size_bytes throughput_GBs\n");

    for (int s = 0; s < NSIZES; s++) {
        size_t sz = SIZES[s];

        for (int i = 0; i < WARMUP; i++) {
            fprintf(stderr, "[lat_client] posting read %d\n", i); fflush(stderr);
            int r = transfer_read(tctx, 0, chunk, meta.vaddr, meta.rkey);
            fprintf(stderr, "[lat_client] post_send ret=%d\n", r); fflush(stderr);
            int p = transfer_poll(tctx, 0, 1);
            fprintf(stderr, "[lat_client] poll ret=%d\n", p); fflush(stderr);
        }

        uint64_t t0 = now_ns();
        for (int i = 0; i < ITERATIONS; i++) {
            transfer_write(tctx, 0, chunk, meta.vaddr, meta.rkey);
            transfer_poll(tctx, 0, 1);
        }
        uint64_t t1 = now_ns();

        double elapsed_s  = (t1 - t0) / 1e9;
        double total_bytes = (double)sz * ITERATIONS;
        double gbps        = total_bytes / elapsed_s / 1e9;

        fprintf(csv, "%zu %.4f\n", sz, gbps);
        fflush(csv);
        printf("[tput] size=%5zuKB  throughput=%.4f GB/s\n", sz/1024, gbps);

        char b = 1; send(msock, &b, 1, 0);
        recv(msock, &b, 1, MSG_WAITALL);
    }

    close(msock);
    char done = 1; write(sync, &done, 1);
    mr_pool_free(tctx->mrp, chunk);
    transfer_destroy(tctx);
}

static void bench_throughput() {
    printf("\n=== Throughput Benchmark ===\n");
    FILE *csv = csv_open("tput.csv");

    int sv[2], cv[2];
    make_sync_pair(sv); make_sync_pair(cv);

    pid_t spid = fork();
    if (spid == 0) { close(sv[0]); tput_server(sv[1]); exit(0); }

    pid_t cpid = fork();
    if (cpid == 0) { close(cv[0]); tput_client(cv[1], csv); exit(0); }

    char b; read(sv[0],&b,1); read(cv[0],&b,1);
    waitpid(spid,nullptr,0); waitpid(cpid,nullptr,0);
    fclose(csv);
    gnuplot_tput("tput.csv");
}

// ------------------------------------------------------------------ //
//  Benchmark 3: MR Pool vs per-transfer ibv_reg_mr
// ------------------------------------------------------------------ //
static void bench_mr_pool() {
    printf("\n=== MR Pool vs Per-transfer Registration ===\n");
    FILE *csv = csv_open("mr.csv");
    fprintf(csv, "# size_bytes pool_alloc_us reg_mr_us\n");

    // need a pd
    int num_devs = 0;
    struct ibv_device **devs = ibv_get_device_list(&num_devs);
    assert(devs && num_devs > 0);
    struct ibv_device *dev = nullptr;
    for (int i = 0; i < num_devs; i++) {
        if (std::string(ibv_get_device_name(devs[i])) == DEV) {
            dev = devs[i]; break;
        }
    }
    assert(dev);
    struct ibv_context *ctx = ibv_open_device(dev);
    ibv_free_device_list(devs);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    for (int s = 0; s < NSIZES; s++) {
        size_t sz = SIZES[s];

        // pool alloc cost
        mr_pool *pool = mr_pool_init(pd, sz, 4);
        assert(pool);
        std::vector<uint64_t> pool_samples;
        for (int i = 0; i < ITERATIONS; i++) {
            uint64_t t0 = now_ns();
            mr_chunk *c = mr_pool_alloc(pool);
            uint64_t t1 = now_ns();
            pool_samples.push_back(t1 - t0);
            mr_pool_free(pool, c);
        }
        mr_pool_destroy(pool);

        // per-transfer ibv_reg_mr cost
        std::vector<uint64_t> reg_samples;
        void *buf = malloc(sz);
        for (int i = 0; i < ITERATIONS; i++) {
            uint64_t t0 = now_ns();
            struct ibv_mr *mr = ibv_reg_mr(pd, buf, sz,
                IBV_ACCESS_LOCAL_WRITE |
                IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_REMOTE_READ);
            uint64_t t1 = now_ns();
            reg_samples.push_back(t1 - t0);
            ibv_dereg_mr(mr);
        }
        free(buf);

        uint64_t pool_p50 = percentile(pool_samples, 50.0) / 1000;
        uint64_t reg_p50  = percentile(reg_samples,  50.0) / 1000;

        fprintf(csv, "%zu %lu %lu\n", sz, pool_p50, reg_p50);
        fflush(csv);
        printf("[mr] size=%5zuKB  pool_alloc=%4luus  ibv_reg_mr=%4luus\n",
               sz/1024, pool_p50, reg_p50);
    }

    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    fclose(csv);
    gnuplot_mr("mr.csv");
}

// ------------------------------------------------------------------ //
//  Benchmark 4: QP reuse vs per-transfer QP setup
// ------------------------------------------------------------------ //
static void bench_qp_reuse() {
    printf("\n=== QP Reuse vs Per-transfer QP Setup ===\n");
    FILE *csv = csv_open("qp.csv");
    fprintf(csv, "# iter reused_us new_us\n");

    int num_devs = 0;
    struct ibv_device **devs = ibv_get_device_list(&num_devs);
    assert(devs && num_devs > 0);
    struct ibv_device *dev = nullptr;
    for (int i = 0; i < num_devs; i++) {
        if (std::string(ibv_get_device_name(devs[i])) == DEV) {
            dev = devs[i]; break;
        }
    }
    assert(dev);
    struct ibv_context *ctx = ibv_open_device(dev);
    ibv_free_device_list(devs);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    // reused QP — create once, measure get_or_create on subsequent calls
    qp_pool *pool = qp_pool_init(ctx, pd);
    qp_pool_get_or_create(pool, 0); // create once

    int N = 200;
    for (int i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        qp_pool_get_or_create(pool, 0); // just a map lookup
        uint64_t t1 = now_ns();

        // new QP every iteration
        qp_pool *tmp = qp_pool_init(ctx, pd);
        uint64_t t2 = now_ns();
        qp_pool_get_or_create(tmp, 99);
        uint64_t t3 = now_ns();
        qp_pool_destroy(tmp);

        fprintf(csv, "%d %lu %lu\n",
                i,
                (t1-t0)/1000,
                (t3-t2)/1000);
    }

    fflush(csv);
    qp_pool_destroy(pool);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    fclose(csv);
    gnuplot_qp("qp.csv");
}

// ------------------------------------------------------------------ //
//  Main
// ------------------------------------------------------------------ //
int main(int argc, char *argv[]) {
    srand(42);

    if (argc >= 3 && !strcmp(argv[1], "--child-lat-server")) {
        int fd = atoi(argv[2]);
        lat_server(fd); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--child-lat-client")) {
        int fd = atoi(argv[2]);
        FILE *csv = csv_open("lat.csv");
        lat_client(fd, csv);
        fclose(csv); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--child-tput-server")) {
        int fd = atoi(argv[2]);
        tput_server(fd); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--child-tput-client")) {
        int fd = atoi(argv[2]);
        FILE *csv = csv_open("tput.csv");
        tput_client(fd, csv);
        fclose(csv); return 0;
    }

    bool run_lat  = false, run_tput = false;
    bool run_mr   = false, run_qp   = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lat"))       run_lat  = true;
        else if (!strcmp(argv[i], "--tput")) run_tput = true;
        else if (!strcmp(argv[i], "--mr"))   run_mr   = true;
        else if (!strcmp(argv[i], "--qp"))   run_qp   = true;
        else if (!strcmp(argv[i], "--all"))  run_lat = run_tput = run_mr = run_qp = true;
    }

    if (!run_lat && !run_tput && !run_mr && !run_qp) {
        fprintf(stderr, "usage: %s --lat | --tput | --mr | --qp | --all\n", argv[0]);
        return 1;
    }

    if (run_mr)   bench_mr_pool();
    if (run_qp)   bench_qp_reuse();
    if (run_lat)  bench_latency();
    if (run_tput) bench_throughput();

    return 0;
}