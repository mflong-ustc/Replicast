/*
 * rdma_allreduce.c — RoCEv2 Ring AllReduce
 *
 * Features:
 *   - K RDMA nodes form a ring (rank r's right neighbor = (r+1)%K, left = (r-1+K)%K)
 *   - Classic ring allreduce: Scatter-Reduce (K-1 steps) + AllGather (K-1 steps)
 *   - Each node sums the data block S = num_msgs × msg_size (uint64 word-wise add)
 *   - One-way ring flow: each step send one chunk to the right, recv one from the left
 *   - Sync: data RDMA WRITE + one flag per QP, receiver polls the flag
 *   - Same as rdma_example / rdma_alltoall: -d/-g/-n/-m/-s/-t args, -N node count,
 *     -S server(rank0) arbitrary, -C -i client, every node warms up every QP
 *
 * Output: per-node FCT / bandwidth, overall allreduce FCT and aggregate bandwidth, plus result verification.
 *
 * Run example (4 ports: 10.1.1.1/10.1.1.2=host1, 10.1.1.3/10.1.1.4=host2):
 *   # host2 server (10.1.1.3):
 *   ./rdma_allreduce -S -d mlx5_0 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *   # the other three clients:
 *   ./rdma_allreduce -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *   ./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *   ./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
 */

#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <stdint.h>

#define DEFAULT_GID_INDEX   3
#define DEFAULT_NUM_QPS     1
#define DEFAULT_NUM_MSGS    100
#define DEFAULT_MSG_SIZE    (1000 * 1024)   /* 1000 KiB ≈ 1 MiB */
#define DEFAULT_TCP_PORT    19876
#define DEFAULT_NUM_NODES   4
#define MAX_DEV_NAME        64
#define MAX_NODES           16
#define MAX_QPS             64

#define MR_ACCESS_FLAGS  (IBV_ACCESS_LOCAL_WRITE  | \
                          IBV_ACCESS_REMOTE_READ  | \
                          IBV_ACCESS_REMOTE_WRITE | \
                          IBV_ACCESS_REMOTE_ATOMIC)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define MOD(x, n) (((x) % (n) + (n)) % (n))

/* ================================================================
 *  Configuration
 * ================================================================ */
struct allreduce_config {
    char   dev_name[MAX_DEV_NAME];
    int    gid_index;
    int    num_qps;       /* QPs per ring direction (per neighbor) */
    int    num_msgs;      /* msgs per node */
    int    msg_size;      /* msg size (bytes) */
    int    mtu;
    int    tcp_port;
    int    num_nodes;     /* K */
    char   server_ip[64];
    int    is_server;
};

/* ================================================================
 *  QP connection info
 * ================================================================ */
struct qp_conn_info {
    uint16_t     lid;
    uint32_t     qp_num;
    uint32_t     psn;
    union ibv_gid gid;
};

/* ================================================================
 *  Per-QP context (out direction writes, in direction receives passively)
 * ================================================================ */
struct qp_ctx {
    struct ibv_cq      *cq;
    struct ibv_qp      *qp;
    struct qp_conn_info local;
    struct qp_conn_info remote;
    /* send-engine scratch (out direction only): 2 WRs per step (data + flag) */
    struct ibv_send_wr wrs[2];
    struct ibv_sge     sges[2];
};

/* ================================================================
 *  Global context
 * ================================================================ */
struct allreduce_context {
    struct ibv_device      *ib_dev;
    struct ibv_context     *context;
    struct ibv_pd          *pd;
    struct ibv_port_attr    port_attr;
    struct ibv_device_attr  dev_attr;
    int                     port_num;
    int                     max_send_wr;

    struct allreduce_config cfg;

    /* Topology */
    int      num_nodes;              /* K */
    int      my_rank;
    int      left_rank;
    int      right_rank;
    uint32_t node_ips[MAX_NODES];

    uint16_t     own_lid;
    union ibv_gid own_gid;
    uint32_t     own_ip;

    /* Data */
    int  data_size;      /* S = num_msgs * msg_size */
    int  chunk_size;     /* C = S / K */
    int  slice_size;     /* C / num_qps */
    char *data_buf;      /* S bytes, this node's data (reduce target + send source) */
    struct ibv_mr *data_mr;

    char *recv_buf;      /* 2 × (C + num_qps*8) bytes: double-buffered recv region */
    struct ibv_mr *recv_mr;
    int  buf_stride;     /* C + num_qps*8, stride of each buffer */

    char *flag_src;      /* 8 bytes, value 1 (flag write source) */
    struct ibv_mr *flag_src_mr;

    /* QP: out_qps[q] = me → right neighbor; in_qps[q] = left neighbor → me */
    struct qp_ctx *out_qps;
    struct qp_ctx *in_qps;

    /* right neighbor's recv buffer (my write target) */
    uint64_t right_recv_addr;
    uint32_t right_recv_rkey;

    /* TCP */
    int listen_fd;
    int client_fds[MAX_NODES];
    int tcp_fd;
};

static struct allreduce_context *g_ctx = NULL;

/* ================================================================
 *  Timing / error / MTU
 * ================================================================ */
static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static inline void die(const char *msg) {
    fprintf(stderr, "ERROR: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static inline int mtu_enum_to_bytes(enum ibv_mtu mtu) {
    switch (mtu) {
        case IBV_MTU_256:  return 256;
        case IBV_MTU_512:  return 512;
        case IBV_MTU_1024: return 1024;
        case IBV_MTU_2048: return 2048;
        case IBV_MTU_4096: return 4096;
        default:           return 1024;
    }
}

static inline enum ibv_mtu mtu_bytes_to_enum(int bytes) {
    switch (bytes) {
        case 256:  return IBV_MTU_256;
        case 512:  return IBV_MTU_512;
        case 1024: return IBV_MTU_1024;
        case 2048: return IBV_MTU_2048;
        case 4096: return IBV_MTU_4096;
        default:   return IBV_MTU_1024;
    }
}

/* ================================================================
 *  Device discovery
 * ================================================================ */
static struct ibv_device *find_device(const char *dev_name) {
    struct ibv_device **dev_list;
    int num_devices;

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0)
        die("No RDMA devices found. Is the driver loaded?");

    printf("Available RDMA devices:\n");
    for (int i = 0; i < num_devices; i++)
        printf("  [%d] %s\n", i, ibv_get_device_name(dev_list[i]));

    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            printf("Using device: %s\n", dev_name);
            return dev_list[i];
        }
    }
    fprintf(stderr, "Device '%s' not found.\n", dev_name);
    ibv_free_device_list(dev_list);
    exit(EXIT_FAILURE);
}

/* ================================================================
 *  TCP reliable read/write
 * ================================================================ */
static int tcp_read_full(int fd, void *buf, size_t len) {
    size_t remain = len;
    char *ptr = (char *)buf;
    while (remain > 0) {
        ssize_t n = read(fd, ptr, remain);
        if (n == 0) { fprintf(stderr, "TCP read: peer closed (EOF)\n"); return -1; }
        if (n < 0)  { fprintf(stderr, "TCP read: %s\n", strerror(errno)); return -1; }
        remain -= (size_t)n; ptr += n;
    }
    return 0;
}

static int tcp_write_full(int fd, const void *buf, size_t len) {
    size_t remain = len;
    const char *ptr = (const char *)buf;
    while (remain > 0) {
        ssize_t n = write(fd, ptr, remain);
        if (n <= 0) return -1;
        remain -= (size_t)n; ptr += n;
    }
    return 0;
}

/* ================================================================
 *  QP state machine
 * ================================================================ */
static void modify_qp_to_init(struct ibv_qp *qp, int port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = (uint8_t)port_num;
    attr.qp_access_flags = MR_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        die("modify_qp_to_init failed");
}

static void modify_qp_to_rtr(struct ibv_qp *qp, struct qp_conn_info *remote,
                             int port_num, int gid_index, enum ibv_mtu mtu) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state               = IBV_QPS_RTR;
    attr.path_mtu               = mtu;
    attr.dest_qp_num            = remote->qp_num;
    attr.rq_psn                 = remote->psn;
    attr.max_dest_rd_atomic     = 16;
    attr.min_rnr_timer          = 12;
    attr.ah_attr.is_global      = 1;
    attr.ah_attr.grh.dgid       = remote->gid;
    attr.ah_attr.grh.sgid_index = (uint8_t)gid_index;
    attr.ah_attr.grh.hop_limit  = 64;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.dlid           = remote->lid;
    attr.ah_attr.sl             = 0;
    attr.ah_attr.src_path_bits  = 0;
    attr.ah_attr.port_num       = (uint8_t)port_num;

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        fprintf(stderr, "ERROR: modify_qp_to_rtr failed: ret=%d (%s)\n", ret, strerror(ret));
        exit(EXIT_FAILURE);
    }
}

static void modify_qp_to_rts(struct ibv_qp *qp, uint32_t sq_psn) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = sq_psn;
    attr.max_rd_atomic = 16;

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        fprintf(stderr, "ERROR: modify_qp_to_rts failed: ret=%d (%s)\n", ret, strerror(ret));
        exit(EXIT_FAILURE);
    }
}

/* ================================================================
 *  Parse command-line args
 * ================================================================ */
static void parse_args(int argc, char *argv[], struct allreduce_config *cfg) {
    int opt;
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->dev_name, "mlx5_0", MAX_DEV_NAME - 1);
    cfg->gid_index  = DEFAULT_GID_INDEX;
    cfg->num_qps    = DEFAULT_NUM_QPS;
    cfg->num_msgs   = DEFAULT_NUM_MSGS;
    cfg->msg_size   = DEFAULT_MSG_SIZE;
    cfg->mtu        = 0;
    cfg->tcp_port   = DEFAULT_TCP_PORT;
    cfg->num_nodes  = DEFAULT_NUM_NODES;
    cfg->is_server  = -1;
    cfg->server_ip[0] = '\0';

    while ((opt = getopt(argc, argv, "d:g:n:m:s:t:p:N:i:SCh")) != -1) {
        switch (opt) {
        case 'd': strncpy(cfg->dev_name, optarg, MAX_DEV_NAME - 1); break;
        case 'g': cfg->gid_index = atoi(optarg); break;
        case 'n':
            cfg->num_qps = atoi(optarg);
            if (cfg->num_qps < 1) cfg->num_qps = 1;
            if (cfg->num_qps > MAX_QPS) { fprintf(stderr, "num_qps too large (max %d)\n", MAX_QPS); exit(1); }
            break;
        case 'm':
            cfg->num_msgs = atoi(optarg);
            if (cfg->num_msgs < 1) cfg->num_msgs = 1;
            break;
        case 's':
            cfg->msg_size = atoi(optarg);
            if (cfg->msg_size < 1) cfg->msg_size = 1;
            break;
        case 't': cfg->mtu = atoi(optarg); break;
        case 'p': cfg->tcp_port = atoi(optarg); break;
        case 'N':
            cfg->num_nodes = atoi(optarg);
            if (cfg->num_nodes < 2) { fprintf(stderr, "num_nodes must be at least 2 (a ring needs at least 2 nodes)\n"); exit(1); }
            if (cfg->num_nodes > MAX_NODES) { fprintf(stderr, "num_nodes too large (max %d)\n", MAX_NODES); exit(1); }
            break;
        case 'i': strncpy(cfg->server_ip, optarg, 63); break;
        case 'S': cfg->is_server = 1; break;
        case 'C': cfg->is_server = 0; break;
        case 'h':
        default:
            printf("Usage: %s -S|-C [options]\n", argv[0]);
            printf("\n=== Common options ===\n");
            printf("  -d <dev>    device           (default: mlx5_0)\n");
            printf("  -g <idx>    GID index        (default: 3)\n");
            printf("  -n <n>      QPs (per ring direction) (default: 1)\n");
            printf("  -m <n>      msgs (per node)  (default: 100)\n");
            printf("  -s <bytes>  msg size         (default: %d)\n", DEFAULT_MSG_SIZE);
            printf("  -t <bytes>  MTU              (default: from ibv_devinfo)\n");
            printf("  -p <port>   TCP port         (default: %d)\n", DEFAULT_TCP_PORT);
            printf("  -N <k>      node count       (default: 4, max: %d)\n", MAX_NODES);
            printf("\n=== Role (choose one) ===\n");
            printf("  -S          server mode (rank 0)\n");
            printf("  -C          client mode (with -i)\n");
            printf("  -i <ip>     server IP\n");
            printf("  -h          help\n");
            exit(0);
        }
    }

    if (cfg->is_server == -1) {
        if (cfg->server_ip[0] != '\0') cfg->is_server = 0;
        else { fprintf(stderr, "ERROR: specify -S or -C -i <server_ip>\n"); exit(1); }
    }
    if (cfg->is_server == 0 && cfg->server_ip[0] == '\0') {
        fprintf(stderr, "ERROR: client must specify server IP via -i\n");
        exit(1);
    }
}

static void print_config(struct allreduce_config *cfg) {
    printf("Configuration:\n");
    printf("  Role:        %s\n", cfg->is_server ? "SERVER (rank 0)" : "CLIENT");
    printf("  Device:      %s\n", cfg->dev_name);
    printf("  GID index:   %d\n", cfg->gid_index);
    printf("  QP count:    %d (per ring direction)\n", cfg->num_qps);
    printf("  Msg count:   %d (per node)\n", cfg->num_msgs);
    printf("  Msg size:    %d bytes (%.2f KiB)\n", cfg->msg_size, (double)cfg->msg_size / 1024.0);
    printf("  Nodes:       %d\n", cfg->num_nodes);
    if (cfg->mtu > 0) printf("  MTU:         %d (user-specified)\n", cfg->mtu);
    else             printf("  MTU:         (from ibv_devinfo)\n");
    if (!cfg->is_server) printf("  Server IP:   %s\n", cfg->server_ip);
    printf("  TCP port:    %d\n\n", cfg->tcp_port);
}

/* ================================================================
 *  Set up RDMA context (no QP/buffers yet)
 * ================================================================ */
static void setup_rdma_context(struct allreduce_context *ctx) {
    ctx->ib_dev = find_device(ctx->cfg.dev_name);
    ctx->context = ibv_open_device(ctx->ib_dev);
    if (!ctx->context) die("ibv_open_device failed");
    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) die("ibv_alloc_pd failed");
    ctx->port_num = 1;

    if (ibv_query_device(ctx->context, &ctx->dev_attr)) die("ibv_query_device failed");
    printf("Device caps: max_qp_wr=%d, max_cqe=%d, max_mr_size=0x%lx\n",
           ctx->dev_attr.max_qp_wr, ctx->dev_attr.max_cqe, ctx->dev_attr.max_mr_size);

    if (ibv_query_port(ctx->context, ctx->port_num, &ctx->port_attr)) die("ibv_query_port failed");
    ctx->own_lid = ctx->port_attr.lid;

    printf("GID table for %s (port %d):\n", ctx->cfg.dev_name, ctx->port_num);
    for (int g = 0; g < 16; g++) {
        union ibv_gid gid;
        if (ibv_query_gid(ctx->context, ctx->port_num, g, &gid)) break;
        uint32_t *raw = (uint32_t *)gid.raw;
        if (raw[0] == 0 && raw[1] == 0 && ntohl(raw[2]) == 0x0000ffff) {
            struct in_addr ip; ip.s_addr = raw[3];
            printf("  %-5d  ::ffff:%s  <- RoCEv2\n", g, inet_ntoa(ip));
        }
    }
    printf("  Using GID index %d\n", ctx->cfg.gid_index);

    if (ibv_query_gid(ctx->context, ctx->port_num, ctx->cfg.gid_index, &ctx->own_gid))
        die("ibv_query_gid failed");
    {
        int zero = 1;
        for (int k = 0; k < 16; k++) if (ctx->own_gid.raw[k] != 0) { zero = 0; break; }
        if (zero) { fprintf(stderr, "ERROR: GID index %d is all zeros\n", ctx->cfg.gid_index); exit(1); }
    }
    ctx->own_ip = ((uint32_t *)ctx->own_gid.raw)[3];
    {
        struct in_addr ip; ip.s_addr = ctx->own_ip;
        printf("  My IP:     %s\n", inet_ntoa(ip));
    }

    printf("  Port state: %s\n  Link layer: %s\n",
           ctx->port_attr.state == IBV_PORT_ACTIVE ? "ACTIVE" :
           ctx->port_attr.state == IBV_PORT_DOWN   ? "DOWN" : "UNKNOWN",
           ctx->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET ? "Ethernet" :
           ctx->port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "Unknown");

    if (ctx->cfg.mtu <= 0) {
        ctx->cfg.mtu = mtu_enum_to_bytes(ctx->port_attr.active_mtu);
        printf("Using port active MTU: %d bytes\n", ctx->cfg.mtu);
    } else {
        printf("Using user-specified MTU: %d bytes\n", ctx->cfg.mtu);
    }

    ctx->max_send_wr = 256;
    if (ctx->dev_attr.max_qp_wr < ctx->max_send_wr) ctx->max_send_wr = (int)ctx->dev_attr.max_qp_wr;
    printf("Per-QP max send WR: %d (device max: %d)\n", ctx->max_send_wr, ctx->dev_attr.max_qp_wr);

    srand((unsigned int)time(NULL));
}

/* ================================================================
 *  Allocate data buffers / QPs / exchange structures
 * ================================================================ */
static void alloc_resources(struct allreduce_context *ctx) {
    int K  = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;
    int S  = ctx->cfg.num_msgs * ctx->cfg.msg_size;
    int C  = S / K;

    /* Validation */
    if (ctx->cfg.msg_size % 8 != 0) {
        fprintf(stderr, "ERROR: msg_size must be a multiple of 8 (uint64 reduction), current %d\n", ctx->cfg.msg_size);
        exit(1);
    }
    if (S % K != 0) {
        fprintf(stderr, "ERROR: num_msgs*msg_size must be divisible by the node count %d, current %d\n", K, S);
        exit(1);
    }
    if (C % 8 != 0) {
        fprintf(stderr, "ERROR: chunk size %d must be a multiple of 8\n", C);
        exit(1);
    }
    if (C % nq != 0) {
        fprintf(stderr, "ERROR: chunk size %d must be divisible by the QP count %d\n", C, nq);
        exit(1);
    }

    ctx->data_size  = S;
    ctx->chunk_size = C;
    ctx->slice_size = C / nq;

    /* Data buffer */
    ctx->data_buf = (char *)calloc(1, S);
    if (!ctx->data_buf) die("calloc data_buf");
    ctx->data_mr = ibv_reg_mr(ctx->pd, ctx->data_buf, S, MR_ACCESS_FLAGS);
    if (!ctx->data_mr) die("ibv_reg_mr data_buf");

    /* fill data: word w = (rank+1) * (w+1), for easy verification */
    uint64_t *dw = (uint64_t *)ctx->data_buf;
    for (int w = 0; w < S / 8; w++)
        dw[w] = (uint64_t)(ctx->my_rank + 1) * (uint64_t)(w + 1);

    /* recv buffer: double-buffered, each block C data + nq*8 flags (alternate by step parity to avoid write/read race) */
    ctx->buf_stride = C + nq * 8;
    ctx->recv_buf = (char *)calloc(1, 2 * ctx->buf_stride);
    if (!ctx->recv_buf) die("calloc recv_buf");
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, 2 * ctx->buf_stride, MR_ACCESS_FLAGS);
    if (!ctx->recv_mr) die("ibv_reg_mr recv_buf");
    for (int b = 0; b < 2; b++)
        for (int q = 0; q < nq; q++)
            ((volatile uint64_t *)(ctx->recv_buf + (size_t)b * ctx->buf_stride + C))[q] = 0;

    /* flag write source (value always 1) */
    ctx->flag_src = (char *)calloc(1, 8);
    if (!ctx->flag_src) die("calloc flag_src");
    *(uint64_t *)ctx->flag_src = 1;
    ctx->flag_src_mr = ibv_reg_mr(ctx->pd, ctx->flag_src, 8, MR_ACCESS_FLAGS);
    if (!ctx->flag_src_mr) die("ibv_reg_mr flag_src");

    /* QP arrays */
    ctx->out_qps = (struct qp_ctx *)calloc(nq, sizeof(struct qp_ctx));
    ctx->in_qps  = (struct qp_ctx *)calloc(nq, sizeof(struct qp_ctx));
    if (!ctx->out_qps || !ctx->in_qps) die("calloc qps");
}

static void create_qp(struct allreduce_context *ctx, struct qp_ctx *Q) {
    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));

    Q->cq = ibv_create_cq(ctx->context, 64, NULL, NULL, 0);
    if (!Q->cq) die("ibv_create_cq failed");

    attr.send_cq = Q->cq;
    attr.recv_cq = Q->cq;
    attr.cap.max_send_wr  = ctx->max_send_wr;
    attr.cap.max_recv_wr  = 16;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.qp_type = IBV_QPT_RC;

    Q->qp = ibv_create_qp(ctx->pd, &attr);
    if (!Q->qp) die("ibv_create_qp failed");

    Q->local.lid    = ctx->own_lid;
    Q->local.qp_num = Q->qp->qp_num;
    Q->local.psn    = (uint32_t)(rand() & 0xFFFFFF);
    Q->local.gid    = ctx->own_gid;

    modify_qp_to_init(Q->qp, ctx->port_num);
}

/* ================================================================
 *  TCP server / client
 * ================================================================ */
static void server_accept_clients(struct allreduce_context *ctx) {
    int K = ctx->cfg.num_nodes;
    int listen_fd, optval = 1;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket() failed");
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)ctx->cfg.tcp_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind() failed");
    if (listen(listen_fd, K) < 0) die("listen() failed");
    ctx->listen_fd = listen_fd;

    printf("[Server] TCP listening on 0.0.0.0:%d, waiting for %d client(s)...\n",
           ctx->cfg.tcp_port, K - 1);

    ctx->node_ips[0] = ctx->own_ip;

    struct { uint32_t ip; int nnodes, nqps, msgsz, nmsgs; } reg;
    for (int r = 1; r < K; r++) {
        struct sockaddr_in caddr;
        socklen_t alen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &alen);
        if (cfd < 0) die("accept() failed");
        if (tcp_read_full(cfd, &reg, sizeof(reg))) die("tcp read reg");
        {
            struct in_addr rip; rip.s_addr = reg.ip;
            printf("[Server] Client #%d (rank %d): GID-IP=%s | nodes=%d, QPs=%d, msg_size=%d, num_msgs=%d\n",
                   r, r, inet_ntoa(rip), reg.nnodes, reg.nqps, reg.msgsz, reg.nmsgs);
        }
        if (reg.nnodes != ctx->cfg.num_nodes || reg.nqps != ctx->cfg.num_qps ||
            reg.msgsz != ctx->cfg.msg_size || reg.nmsgs != ctx->cfg.num_msgs) {
            fprintf(stderr, "ERROR: client #%d config mismatch!\n", r);
            exit(1);
        }
        ctx->client_fds[r] = cfd;
        ctx->node_ips[r]   = reg.ip;
    }

    printf("[Server] Topology:\n");
    for (int r = 0; r < K; r++) {
        struct in_addr ip; ip.s_addr = ctx->node_ips[r];
        printf("  rank %d: %s%s\n", r, inet_ntoa(ip), r == 0 ? "  (server)" : "");
    }
    printf("\n");
}

static void client_connect(struct allreduce_context *ctx) {
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) die("socket() failed");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)ctx->cfg.tcp_port);
    if (inet_pton(AF_INET, ctx->cfg.server_ip, &addr.sin_addr) != 1) die("Invalid server IP");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("connect() failed");
    ctx->tcp_fd = sock;
    printf("[Client] TCP connected to %s:%d\n", ctx->cfg.server_ip, ctx->cfg.tcp_port);

    struct { uint32_t ip; int nnodes, nqps, msgsz, nmsgs; } reg;
    reg.ip = ctx->own_ip; reg.nnodes = ctx->cfg.num_nodes; reg.nqps = ctx->cfg.num_qps;
    reg.msgsz = ctx->cfg.msg_size; reg.nmsgs = ctx->cfg.num_msgs;
    if (tcp_write_full(sock, &reg, sizeof(reg))) die("tcp write reg");

    struct { int nnodes, rank, nqps, msgsz, nmsgs; uint32_t ips[MAX_NODES]; } cm;
    if (tcp_read_full(sock, &cm, sizeof(cm))) die("tcp read config");
    ctx->num_nodes = cm.nnodes;
    ctx->my_rank   = cm.rank;
    ctx->cfg.num_qps  = cm.nqps;
    ctx->cfg.msg_size = cm.msgsz;
    ctx->cfg.num_msgs = cm.nmsgs;
    memcpy(ctx->node_ips, cm.ips, MAX_NODES * sizeof(uint32_t));

    printf("[Client] My rank=%d, nodes=%d. Topology:\n", ctx->my_rank, ctx->num_nodes);
    for (int r = 0; r < ctx->num_nodes; r++) {
        struct in_addr ip; ip.s_addr = ctx->node_ips[r];
        printf("  rank %d: %s%s\n", r, inet_ntoa(ip), r == ctx->my_rank ? "  (me)" : "");
    }
    printf("\n");
}

/* ================================================================
 *  Centralized ring info exchange
 *
 *  out_conns[r][q] = node r's out QP q info (sent to left neighbor)
 *  in_conns[r][q]  = node r's in  QP q info + recv buffer (sent to right neighbor)
 *  Node r needs: out_conns[left][q] (for in QP), in_conns[right][q] + right recv (for out QP)
 * ================================================================ */
static void exchange_ring_info(struct allreduce_context *ctx) {
    int K = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;

    static struct qp_conn_info out_conns[MAX_NODES][MAX_QPS];
    static struct qp_conn_info in_conns[MAX_NODES][MAX_QPS];
    static uint64_t recv_addr[MAX_NODES];
    static uint32_t recv_rkey[MAX_NODES];

    if (ctx->cfg.is_server) {
        /* server itself */
        for (int q = 0; q < nq; q++) {
            out_conns[0][q] = ctx->out_qps[q].local;
            in_conns[0][q]  = ctx->in_qps[q].local;
        }
        recv_addr[0] = (uint64_t)(uintptr_t)ctx->recv_buf;
        recv_rkey[0] = ctx->recv_mr->rkey;

        /* receive client reports */
        for (int r = 1; r < K; r++) {
            if (tcp_read_full(ctx->client_fds[r], out_conns[r], nq * sizeof(struct qp_conn_info))) die("read out");
            if (tcp_read_full(ctx->client_fds[r], in_conns[r], nq * sizeof(struct qp_conn_info))) die("read in");
            if (tcp_read_full(ctx->client_fds[r], &recv_addr[r], sizeof(uint64_t))) die("read addr");
            if (tcp_read_full(ctx->client_fds[r], &recv_rkey[r], sizeof(uint32_t))) die("read rkey");
        }

        /* distribute to each client */
        for (int r = 1; r < K; r++) {
            int left  = MOD(r - 1, K);
            int right = MOD(r + 1, K);
            if (tcp_write_full(ctx->client_fds[r], out_conns[left], nq * sizeof(struct qp_conn_info))) die("write out");
            if (tcp_write_full(ctx->client_fds[r], in_conns[right], nq * sizeof(struct qp_conn_info))) die("write in");
            if (tcp_write_full(ctx->client_fds[r], &recv_addr[right], sizeof(uint64_t))) die("write addr");
            if (tcp_write_full(ctx->client_fds[r], &recv_rkey[right], sizeof(uint32_t))) die("write rkey");
        }

        /* server applies: in QP ← left's out, out QP ← right's in */
        int left = MOD(0 - 1, K), right = MOD(0 + 1, K);
        for (int q = 0; q < nq; q++) {
            ctx->in_qps[q].remote  = out_conns[left][q];
            ctx->out_qps[q].remote = in_conns[right][q];
        }
        ctx->right_recv_addr = recv_addr[right];
        ctx->right_recv_rkey = recv_rkey[right];
    } else {
        int me = ctx->my_rank;
        /* report local */
        for (int q = 0; q < nq; q++) {
            out_conns[me][q] = ctx->out_qps[q].local;
            in_conns[me][q]  = ctx->in_qps[q].local;
        }
        uint64_t my_addr = (uint64_t)(uintptr_t)ctx->recv_buf;
        uint32_t my_rkey = ctx->recv_mr->rkey;
        if (tcp_write_full(ctx->tcp_fd, out_conns[me], nq * sizeof(struct qp_conn_info))) die("write out");
        if (tcp_write_full(ctx->tcp_fd, in_conns[me], nq * sizeof(struct qp_conn_info))) die("write in");
        if (tcp_write_full(ctx->tcp_fd, &my_addr, sizeof(uint64_t))) die("write addr");
        if (tcp_write_full(ctx->tcp_fd, &my_rkey, sizeof(uint32_t))) die("write rkey");

        /* receive server's routing */
        int left = MOD(me - 1, K), right = MOD(me + 1, K);
        static struct qp_conn_info lbuf[MAX_QPS], rbuf[MAX_QPS];
        if (tcp_read_full(ctx->tcp_fd, lbuf, nq * sizeof(struct qp_conn_info))) die("read out");
        if (tcp_read_full(ctx->tcp_fd, rbuf, nq * sizeof(struct qp_conn_info))) die("read in");
        if (tcp_read_full(ctx->tcp_fd, &ctx->right_recv_addr, sizeof(uint64_t))) die("read addr");
        if (tcp_read_full(ctx->tcp_fd, &ctx->right_recv_rkey, sizeof(uint32_t))) die("read rkey");

        for (int q = 0; q < nq; q++) {
            ctx->in_qps[q].remote  = lbuf[q];   /* left's out */
            ctx->out_qps[q].remote = rbuf[q];   /* right's in */
        }
    }
}

/* ================================================================
 *  Bring all QPs to RTR + RTS (two-phase)
 * ================================================================ */
static void bring_qps_up(struct allreduce_context *ctx) {
    enum ibv_mtu mtu = mtu_bytes_to_enum(ctx->cfg.mtu);
    int nq = ctx->cfg.num_qps;

    for (int q = 0; q < nq; q++) {
        modify_qp_to_rtr(ctx->out_qps[q].qp, &ctx->out_qps[q].remote, ctx->port_num, ctx->cfg.gid_index, mtu);
        modify_qp_to_rtr(ctx->in_qps[q].qp,  &ctx->in_qps[q].remote,  ctx->port_num, ctx->cfg.gid_index, mtu);
    }
    for (int q = 0; q < nq; q++) {
        modify_qp_to_rts(ctx->out_qps[q].qp, ctx->out_qps[q].local.psn);
        modify_qp_to_rts(ctx->in_qps[q].qp,  ctx->in_qps[q].local.psn);
    }
    printf("[rank %d] All QPs -> RTS.\n", ctx->my_rank);
}

/* ================================================================
 *  Warmup: send one 8-byte RDMA WRITE per out QP (also warms the right neighbor's in QP)
 * ================================================================ */
static void warmup(struct allreduce_context *ctx) {
    int nq = ctx->cfg.num_qps;
    printf("[rank %d] Warming up %d out QP(s)...\n", ctx->my_rank, nq);
    for (int q = 0; q < nq; q++) {
        struct ibv_sge sge;
        struct ibv_send_wr wr, *bad;
        struct ibv_wc wc;
        sge.addr = (uintptr_t)ctx->data_buf; sge.length = 8; sge.lkey = ctx->data_mr->lkey;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 0; wr.sg_list = &sge; wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE; wr.send_flags = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = ctx->right_recv_addr;
        wr.wr.rdma.rkey = ctx->right_recv_rkey;
        if (ibv_post_send(ctx->out_qps[q].qp, &wr, &bad)) die("warmup post_send");
        while (ibv_poll_cq(ctx->out_qps[q].cq, 1, &wc) == 0) ;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[rank %d] warmup QP %d failed: %s\n",
                    ctx->my_rank, q, ibv_wc_status_str(wc.status));
            die("warmup failed");
        }
    }
    printf("[rank %d] Warm-up complete.\n", ctx->my_rank);
}

/* ================================================================
 *  Send one chunk to the right neighbor (non-blocking): each QP posts slice + flag WRs
 *  The flag value is a monotonically increasing step, to avoid the boolean-flag lost-update race.
 * ================================================================ */
static void post_send_chunk(struct allreduce_context *ctx, int chunk_idx, uint64_t step) {
    int nq = ctx->cfg.num_qps;
    int C  = ctx->chunk_size;
    int slice = ctx->slice_size;
    size_t buf_off = (size_t)((step - 1) % 2) * ctx->buf_stride;   /* double-buffer offset */

    /* write a different step value each step */
    *(volatile uint64_t *)ctx->flag_src = step;

    for (int q = 0; q < nq; q++) {
        struct qp_ctx *Q = &ctx->out_qps[q];
        int off = q * slice;

        Q->sges[0].addr   = (uintptr_t)(ctx->data_buf + (size_t)chunk_idx * C + off);
        Q->sges[0].length = (uint32_t)slice;
        Q->sges[0].lkey   = ctx->data_mr->lkey;
        memset(&Q->wrs[0], 0, sizeof(Q->wrs[0]));
        Q->wrs[0].wr_id   = 0;
        Q->wrs[0].sg_list = &Q->sges[0];
        Q->wrs[0].num_sge = 1;
        Q->wrs[0].opcode  = IBV_WR_RDMA_WRITE;
        Q->wrs[0].send_flags = 0;
        Q->wrs[0].wr.rdma.remote_addr = ctx->right_recv_addr + buf_off + (size_t)off;
        Q->wrs[0].wr.rdma.rkey = ctx->right_recv_rkey;
        Q->wrs[0].next = &Q->wrs[1];

        Q->sges[1].addr   = (uintptr_t)ctx->flag_src;
        Q->sges[1].length = 8;
        Q->sges[1].lkey   = ctx->flag_src_mr->lkey;
        memset(&Q->wrs[1], 0, sizeof(Q->wrs[1]));
        Q->wrs[1].wr_id   = 1;
        Q->wrs[1].sg_list = &Q->sges[1];
        Q->wrs[1].num_sge = 1;
        Q->wrs[1].opcode  = IBV_WR_RDMA_WRITE;
        Q->wrs[1].send_flags = IBV_SEND_SIGNALED;
        Q->wrs[1].wr.rdma.remote_addr = ctx->right_recv_addr + buf_off + (size_t)C + (size_t)q * 8;
        Q->wrs[1].wr.rdma.rkey = ctx->right_recv_rkey;
        Q->wrs[1].next = NULL;

        struct ibv_send_wr *bad = NULL;
        if (ibv_post_send(Q->qp, &Q->wrs[0], &bad)) die("post_send_chunk failed");
    }
}

static void wait_send_completion(struct allreduce_context *ctx) {
    int nq = ctx->cfg.num_qps;
    for (int q = 0; q < nq; q++) {
        struct ibv_wc wc;
        while (ibv_poll_cq(ctx->out_qps[q].cq, 1, &wc) == 0) ;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[rank %d] send WC error on out QP %d: %s\n",
                    ctx->my_rank, q, ibv_wc_status_str(wc.status));
            exit(EXIT_FAILURE);
        }
    }
}

/* ================================================================
 *  Recv one chunk from the left neighbor and reduce / replace
 *  flag is a monotonically increasing step: wait until flags[q] == step, no reset needed.
 * ================================================================ */
static void recv_from_left(struct allreduce_context *ctx, int chunk_idx, int do_reduce, uint64_t step) {
    int nq = ctx->cfg.num_qps;
    int C  = ctx->chunk_size;
    size_t buf_off = (size_t)((step - 1) % 2) * ctx->buf_stride;   /* double-buffer offset */
    volatile uint64_t *flags = (volatile uint64_t *)(ctx->recv_buf + buf_off + C);
    uint64_t *src = (uint64_t *)(ctx->recv_buf + buf_off);

    /* poll this buffer's flags for all QPs until equal to this step */
    for (int q = 0; q < nq; q++)
        while (flags[q] != step) ;

    if (do_reduce) {
        uint64_t *dst = (uint64_t *)(ctx->data_buf + (size_t)chunk_idx * C);
        for (int w = 0; w < C / 8; w++) dst[w] += src[w];
    } else {
        memcpy(ctx->data_buf + (size_t)chunk_idx * C, src, C);
    }
}

/* ================================================================
 *  Verify: after reduction data_buf[w] should equal K(K+1)/2 * (w+1)
 * ================================================================ */
static int verify_result(struct allreduce_context *ctx) {
    int K = ctx->num_nodes;
    uint64_t factor = (uint64_t)K * (uint64_t)(K + 1) / 2;
    uint64_t *dw = (uint64_t *)ctx->data_buf;
    for (int w = 0; w < ctx->data_size / 8; w++) {
        uint64_t expect = factor * (uint64_t)(w + 1);
        if (dw[w] != expect) {
            printf("[rank %d] VERIFY FAILED at word %d: got %lu, expect %lu\n",
                   ctx->my_rank, w, dw[w], expect);
            return 0;
        }
    }
    return 1;
}

/* ================================================================
 *  Run timed ring allreduce, return this node's FCT (us)
 * ================================================================ */
static double run_timed_allreduce(struct allreduce_context *ctx) {
    int K = ctx->num_nodes;
    int r = ctx->my_rank;

    printf("[rank %d] Starting timed ring allreduce: %d nodes, S=%.2f MiB, C=%.2f KiB\n",
           r, K, (double)ctx->data_size / (1024.0 * 1024.0),
           (double)ctx->chunk_size / 1024.0);

    double start = get_time_us();

    /* ---- Scatter-Reduce: K-1 steps (step value 1..K-1) ---- */
    for (int s = 0; s < K - 1; s++) {
        int send_idx = MOD(r - s, K);
        int recv_idx = MOD(r - s - 1, K);
        uint64_t step = (uint64_t)(s + 1);
        post_send_chunk(ctx, send_idx, step);
        recv_from_left(ctx, recv_idx, 1, step);   /* reduce */
        wait_send_completion(ctx);
    }

    /* ---- AllGather: K-1 steps (step value K..2K-2) ---- */
    for (int s = 0; s < K - 1; s++) {
        int send_idx = MOD(r + 1 - s, K);
        int recv_idx = MOD(r - s, K);
        uint64_t step = (uint64_t)(K + s);
        post_send_chunk(ctx, send_idx, step);
        recv_from_left(ctx, recv_idx, 0, step);   /* replace */
        wait_send_completion(ctx);
    }

    double end = get_time_us();
    return end - start;
}

/* ================================================================
 *  Cleanup
 * ================================================================ */
static void cleanup(struct allreduce_context *ctx) {
    int nq = ctx->cfg.num_qps;
    for (int q = 0; q < nq; q++) {
        if (ctx->out_qps && ctx->out_qps[q].qp) ibv_destroy_qp(ctx->out_qps[q].qp);
        if (ctx->out_qps && ctx->out_qps[q].cq) ibv_destroy_cq(ctx->out_qps[q].cq);
        if (ctx->in_qps  && ctx->in_qps[q].qp)  ibv_destroy_qp(ctx->in_qps[q].qp);
        if (ctx->in_qps  && ctx->in_qps[q].cq)  ibv_destroy_cq(ctx->in_qps[q].cq);
    }
    free(ctx->out_qps);
    free(ctx->in_qps);
    if (ctx->data_mr)     ibv_dereg_mr(ctx->data_mr);
    if (ctx->recv_mr)     ibv_dereg_mr(ctx->recv_mr);
    if (ctx->flag_src_mr) ibv_dereg_mr(ctx->flag_src_mr);
    free(ctx->data_buf);
    free(ctx->recv_buf);
    free(ctx->flag_src);
    if (ctx->pd)      ibv_dealloc_pd(ctx->pd);
    if (ctx->context) ibv_close_device(ctx->context);
}

static void sig_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nInterrupted. Cleaning up...\n");
    if (g_ctx) {
        if (g_ctx->listen_fd > 0) close(g_ctx->listen_fd);
        if (g_ctx->tcp_fd > 0)    close(g_ctx->tcp_fd);
        for (int r = 0; r < MAX_NODES; r++) if (g_ctx->client_fds[r] > 0) close(g_ctx->client_fds[r]);
        cleanup(g_ctx);
    }
    exit(0);
}

/* ================================================================
 *  main
 * ================================================================ */
int main(int argc, char *argv[]) {
    struct allreduce_context ctx;
    double fct_us = 0.0;

    memset(&ctx, 0, sizeof(ctx));
    parse_args(argc, argv, &ctx.cfg);
    g_ctx = &ctx;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║           RoCEv2 Ring AllReduce Generator               ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    print_config(&ctx.cfg);

    setup_rdma_context(&ctx);

    /* Topology */
    if (ctx.cfg.is_server) {
        ctx.num_nodes = ctx.cfg.num_nodes;
        server_accept_clients(&ctx);
        ctx.my_rank = 0;

        struct { int nnodes, rank, nqps, msgsz, nmsgs; uint32_t ips[MAX_NODES]; } cm;
        cm.nnodes = ctx.num_nodes; cm.rank = 0;
        cm.nqps = ctx.cfg.num_qps; cm.msgsz = ctx.cfg.msg_size; cm.nmsgs = ctx.cfg.num_msgs;
        memcpy(cm.ips, ctx.node_ips, MAX_NODES * sizeof(uint32_t));
        for (int r = 1; r < ctx.num_nodes; r++) {
            cm.rank = r;
            if (tcp_write_full(ctx.client_fds[r], &cm, sizeof(cm))) die("tcp write config");
        }
    } else {
        client_connect(&ctx);
    }

    ctx.left_rank  = MOD(ctx.my_rank - 1, ctx.num_nodes);
    ctx.right_rank = MOD(ctx.my_rank + 1, ctx.num_nodes);
    {
        char lip[INET_ADDRSTRLEN], rip[INET_ADDRSTRLEN];
        struct in_addr a;
        a.s_addr = ctx.node_ips[ctx.left_rank];
        inet_ntop(AF_INET, &a, lip, sizeof(lip));
        a.s_addr = ctx.node_ips[ctx.right_rank];
        inet_ntop(AF_INET, &a, rip, sizeof(rip));
        printf("[rank %d] Ring: left=%d (%s), right=%d (%s)\n",
               ctx.my_rank, ctx.left_rank, lip, ctx.right_rank, rip);
    }

    /* allocate resources + create QPs */
    alloc_resources(&ctx);
    for (int q = 0; q < ctx.cfg.num_qps; q++) {
        create_qp(&ctx, &ctx.out_qps[q]);
        create_qp(&ctx, &ctx.in_qps[q]);
    }

    /* exchange ring info */
    exchange_ring_info(&ctx);

    /* RTR/RTS */
    bring_qps_up(&ctx);

    /* warmup */
    warmup(&ctx);

    /* barrier + timing */
    struct { double fct; uint64_t bytes; } res;
    res.bytes = (uint64_t)2 * (ctx.num_nodes - 1) * ctx.chunk_size;

    if (ctx.cfg.is_server) {
        int K = ctx.num_nodes;
        for (int r = 1; r < K; r++) {
            int ready = 0;
            if (tcp_read_full(ctx.client_fds[r], &ready, sizeof(ready))) die("read ready");
        }
        printf("[Server] All clients ready. Broadcasting GO...\n");
        int go = 1;
        for (int r = 1; r < K; r++)
            if (tcp_write_full(ctx.client_fds[r], &go, sizeof(go))) die("write go");

        fct_us = run_timed_allreduce(&ctx);

        struct { double fct; uint64_t bytes; } results[MAX_NODES];
        results[0].fct = fct_us;
        results[0].bytes = res.bytes;
        for (int r = 1; r < K; r++)
            if (tcp_read_full(ctx.client_fds[r], &results[r], sizeof(results[r]))) die("read result");

        double max_fct = 0.0;
        uint64_t total_bytes = 0;
        for (int r = 0; r < K; r++) {
            if (results[r].fct > max_fct) max_fct = results[r].fct;
            total_bytes += results[r].bytes;
        }
        double agg_bw = (double)total_bytes * 8.0 / 1e9 / (max_fct / 1e6);

        printf("\n╔════════════════════════════════════════════════════════════════╗\n");
        printf("║          Ring AllReduce Results (coordinator view)           ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║  rank  %-15s  %12s  %12s  %14s ║\n", "IP", "Bytes Sent", "FCT (us)", "Node BW (Gbps)");
        printf("╠────────────────────────────────────────────────────────────────╣\n");
        for (int r = 0; r < K; r++) {
            struct in_addr ip; ip.s_addr = ctx.node_ips[r];
            double nbw = (double)results[r].bytes * 8.0 / 1e9 / (results[r].fct / 1e6);
            printf("║  %4d  %-15s  %12lu  %12.2f  %14.2f ║\n",
                   r, inet_ntoa(ip), results[r].bytes, results[r].fct, nbw);
        }
        printf("╠────────────────────────────────────────────────────────────────╣\n");
        printf("║  Allreduce data (S):  %.2f MiB per node                     ║\n",
               (double)ctx.data_size / (1024.0 * 1024.0));
        printf("║  Aggregate traffic:   %.2f MiB                              ║\n",
               (double)total_bytes / (1024.0 * 1024.0));
        printf("║  Allreduce FCT (max): %.2f us  (%.3f ms)                    ║\n",
               max_fct, max_fct / 1000.0);
        printf("║  Aggregate bandwidth: %.2f Gbps                             ║\n", agg_bw);
        printf("║  Effective (S/FCT):   %.2f Gbps                             ║\n",
               (double)ctx.data_size * 8.0 / 1e9 / (max_fct / 1e6));
        printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    } else {
        int ready = 1;
        if (tcp_write_full(ctx.tcp_fd, &ready, sizeof(ready))) die("write ready");
        int go = 0;
        if (tcp_read_full(ctx.tcp_fd, &go, sizeof(go))) die("read go");

        fct_us = run_timed_allreduce(&ctx);
        res.fct = fct_us;
        if (tcp_write_full(ctx.tcp_fd, &res, sizeof(res))) die("write result");

        double nbw = (double)res.bytes * 8.0 / 1e9 / (fct_us / 1e6);
        printf("\n[Client rank %d] Allreduce complete:\n", ctx.my_rank);
        printf("  Bytes sent:  %.2f MiB\n", (double)res.bytes / (1024.0 * 1024.0));
        printf("  FCT:         %.2f us  (%.3f ms)\n", fct_us, fct_us / 1000.0);
        printf("  Node BW:     %.2f Gbps\n", nbw);
    }

    /* verify */
    if (verify_result(&ctx))
        printf("[rank %d] Verify OK: allreduce result correct. ✓\n", ctx.my_rank);
    else
        printf("[rank %d] Verify FAILED.\n", ctx.my_rank);

    /* cleanup */
    if (ctx.cfg.is_server) {
        for (int r = 1; r < ctx.num_nodes; r++) if (ctx.client_fds[r] > 0) close(ctx.client_fds[r]);
        if (ctx.listen_fd > 0) close(ctx.listen_fd);
    } else {
        if (ctx.tcp_fd > 0) close(ctx.tcp_fd);
    }
    cleanup(&ctx);

    printf("[rank %d] Done.\n", ctx.my_rank);
    return 0;
}
