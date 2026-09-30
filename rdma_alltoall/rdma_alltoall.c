/*
 * rdma_alltoall.c — RoCEv2 All-to-All Traffic Generator
 *
 * Features:
 *   - Builds a full-mesh of RC QP connections among N RDMA ports (nodes)
 *   - Each node performs RDMA WRITE to the other N-1 nodes (alltoall pattern)
 *   - Configurable: device / GID index / #QPs / msg size / #msgs / MTU / #nodes
 *   - Server (rank 0) is arbitrary; clients are the remaining ports
 *   - Each node warms up every QP (first packet primes the HW pipeline),
 *     then starts timing after a GO barrier
 *   - Output: per-node flow completion time (FCT) and aggregate bandwidth
 *
 * Consistent with rdma_example:
 *   -d device  -g GID  -n QPs(per link)  -s msg size  -m #msgs  -t MTU
 *
 * Added:
 *   -N number of alltoall nodes (default 4)
 *   -S server mode (coordinator, rank 0)  -C client mode (needs -i server IP)
 *
 * Run example (4 ports: 10.1.1.1/10.1.1.2 on host1, 10.1.1.3/10.1.1.4 on host2):
 *   # on host2 start the server (arbitrary, e.g. 10.1.1.3 on mlx5_0):
 *   ./rdma_alltoall -S -d mlx5_0 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *
 *   # on host1 start two clients (10.1.1.1, 10.1.1.2):
 *   ./rdma_alltoall -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *   ./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
 *
 *   # on host2 start the fourth client (10.1.1.4):
 *   ./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
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

/* ================================================================
 *  Defaults / limits
 * ================================================================ */
#define DEFAULT_GID_INDEX   3
#define DEFAULT_NUM_QPS     1
#define DEFAULT_NUM_MSGS    100
#define DEFAULT_MSG_SIZE    (1000 * 1024)   /* 1000 KiB ≈ 1 MiB */
#define DEFAULT_TCP_PORT    19875
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

/* ================================================================
 *  Configuration
 * ================================================================ */
struct alltoall_config {
    char   dev_name[MAX_DEV_NAME];
    int    gid_index;
    int    num_qps;       /* QPs per link (per node pair) */
    int    num_msgs;      /* msgs each node sends to each peer */
    int    msg_size;      /* msg size (bytes) */
    int    mtu;           /* 0 = use port default */
    int    tcp_port;
    int    num_nodes;     /* number of alltoall nodes K */
    char   server_ip[64]; /* client: server IP */
    int    is_server;     /* 1 = server(rank0), 0 = client */
};

/* ================================================================
 *  QP connection info — exchanged over TCP (same as rdma_example)
 * ================================================================ */
struct qp_conn_info {
    uint16_t     lid;
    uint32_t     qp_num;
    uint32_t     psn;
    union ibv_gid gid;
};

/* ================================================================
 *  Per-link (node i → peer j) QP resources
 * ================================================================ */
struct link_ctx {
    struct ibv_cq      *cq;
    struct ibv_qp      *qp;
    struct ibv_mr      *send_mr;   /* local send buffer MR (write source) */
    char               *send_buf;
    struct ibv_mr      *recv_mr;   /* local recv buffer MR (peer write target) */
    char               *recv_buf;
    int                 buf_size;

    struct qp_conn_info local;     /* local qp_num / psn / lid / gid */
    struct qp_conn_info remote;    /* remote qp_num / psn / lid / gid */
    uint64_t            remote_addr; /* remote recv buffer addr (WRITE target) */
    uint32_t            remote_rkey; /* remote recv buffer rkey */

    /* send-engine scratch (at most one batch in flight per link at a time, reusable) */
    struct ibv_send_wr *wrs;
    struct ibv_sge     *sges;
};

/* ================================================================
 *  Global context
 * ================================================================ */
struct alltoall_context {
    struct ibv_device      *ib_dev;
    struct ibv_context     *context;
    struct ibv_pd          *pd;
    struct ibv_port_attr    port_attr;
    struct ibv_device_attr  dev_attr;
    int                     port_num;
    int                     max_send_wr;

    struct alltoall_config  cfg;

    /* Topology */
    int          num_nodes;              /* K */
    int          my_rank;                /* 0..K-1 */
    uint32_t     node_ips[MAX_NODES];    /* rank → IPv4 (s_addr) */

    /* this node's identity (from GID) */
    uint16_t     own_lid;
    union ibv_gid own_gid;
    uint32_t     own_ip;

    /* links: links[peer_rank][qp_idx] */
    struct link_ctx **links;

    /* TCP */
    int listen_fd;                  /* server */
    int client_fds[MAX_NODES];      /* server: rank → client socket */
    int tcp_fd;                     /* client: socket to server */
};

static struct alltoall_context *g_ctx = NULL;

/* ================================================================
 *  Timing / error handling / MTU conversion
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
        if (n == 0) {
            fprintf(stderr, "TCP read: peer closed connection (EOF)\n");
            return -1;
        }
        if (n < 0) {
            fprintf(stderr, "TCP read: %s (errno=%d)\n", strerror(errno), errno);
            return -1;
        }
        remain -= (size_t)n;
        ptr    += n;
    }
    return 0;
}

static int tcp_write_full(int fd, const void *buf, size_t len) {
    size_t remain = len;
    const char *ptr = (const char *)buf;
    while (remain > 0) {
        ssize_t n = write(fd, ptr, remain);
        if (n <= 0) return -1;
        remain -= (size_t)n;
        ptr    += n;
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
                      IBV_QP_STATE      |
                      IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT       |
                      IBV_QP_ACCESS_FLAGS))
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
                            IBV_QP_STATE              |
                            IBV_QP_AV                |
                            IBV_QP_PATH_MTU          |
                            IBV_QP_DEST_QPN          |
                            IBV_QP_RQ_PSN            |
                            IBV_QP_MAX_DEST_RD_ATOMIC|
                            IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        fprintf(stderr, "ERROR: modify_qp_to_rtr failed: ret=%d (%s)\n",
                ret, strerror(ret));
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
                            IBV_QP_STATE      |
                            IBV_QP_TIMEOUT    |
                            IBV_QP_RETRY_CNT  |
                            IBV_QP_RNR_RETRY  |
                            IBV_QP_SQ_PSN     |
                            IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        fprintf(stderr, "ERROR: modify_qp_to_rts failed: ret=%d (%s)\n",
                ret, strerror(ret));
        exit(EXIT_FAILURE);
    }
}

/* ================================================================
 *  Parse command-line args (same as rdma_example, plus -N/-S/-C)
 * ================================================================ */
static void parse_args(int argc, char *argv[], struct alltoall_config *cfg) {
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
    cfg->is_server  = -1;   /* unspecified */
    cfg->server_ip[0] = '\0';

    while ((opt = getopt(argc, argv, "d:g:n:m:s:t:p:N:i:SCh")) != -1) {
        switch (opt) {
        case 'd': strncpy(cfg->dev_name, optarg, MAX_DEV_NAME - 1); break;
        case 'g': cfg->gid_index = atoi(optarg); break;
        case 'n':
            cfg->num_qps = atoi(optarg);
            if (cfg->num_qps < 1) cfg->num_qps = 1;
            if (cfg->num_qps > MAX_QPS) {
                fprintf(stderr, "num_qps too large (max %d)\n", MAX_QPS);
                exit(1);
            }
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
            if (cfg->num_nodes < 1) cfg->num_nodes = 1;
            if (cfg->num_nodes > MAX_NODES) {
                fprintf(stderr, "num_nodes too large (max %d)\n", MAX_NODES);
                exit(1);
            }
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
            printf("  -n <n>      QPs (per link)   (default: 1)\n");
            printf("  -m <n>      msgs (per peer)  (default: 100)\n");
            printf("  -s <bytes>  msg size         (default: %d)\n", DEFAULT_MSG_SIZE);
            printf("  -t <bytes>  MTU              (default: from ibv_devinfo)\n");
            printf("  -p <port>   TCP port         (default: %d)\n", DEFAULT_TCP_PORT);
            printf("  -N <k>      alltoall nodes   (default: 4, max: %d)\n", MAX_NODES);
            printf("\n=== Role (choose one) ===\n");
            printf("  -S          server mode (coordinator, rank 0)\n");
            printf("  -C          client mode (needs -i server IP)\n");
            printf("  -i <ip>     server IP (client only)\n");
            printf("  -h          help\n");
            printf("\nExamples:\n");
            printf("  # server (host2, mlx5_0 / 10.1.1.3):\n");
            printf("  %s -S -d mlx5_0 -g 3 -N 4\n", argv[0]);
            printf("  # client (host1, mlx5_0 / 10.1.1.1):\n");
            printf("  %s -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4\n", argv[0]);
            exit(0);
        }
    }

    /* role inference: if neither -S nor -C given, treat -i as client */
    if (cfg->is_server == -1) {
        if (cfg->server_ip[0] != '\0') cfg->is_server = 0;
        else {
            fprintf(stderr, "ERROR: specify a role: -S (server) or -C -i <server_ip> (client)\n");
            exit(1);
        }
    }
    if (cfg->is_server == 0 && cfg->server_ip[0] == '\0') {
        fprintf(stderr, "ERROR: client must specify server IP via -i\n");
        exit(1);
    }
}

/* ================================================================
 *  Print configuration
 * ================================================================ */
static void print_config(struct alltoall_config *cfg) {
    printf("Configuration:\n");
    printf("  Role:        %s\n", cfg->is_server ? "SERVER (rank 0)" : "CLIENT");
    printf("  Device:      %s\n", cfg->dev_name);
    printf("  GID index:   %d\n", cfg->gid_index);
    printf("  QP count:    %d (per link)\n", cfg->num_qps);
    printf("  Msg count:   %d (per peer)\n", cfg->num_msgs);
    printf("  Msg size:    %d bytes (%.2f KiB)\n",
           cfg->msg_size, (double)cfg->msg_size / 1024.0);
    printf("  Nodes:       %d\n", cfg->num_nodes);
    if (cfg->mtu > 0)
        printf("  MTU:         %d (user-specified)\n", cfg->mtu);
    else
        printf("  MTU:         (from ibv_devinfo)\n");
    if (!cfg->is_server)
        printf("  Server IP:   %s\n", cfg->server_ip);
    printf("  TCP port:    %d\n\n", cfg->tcp_port);
}

/* ================================================================
 *  Set up RDMA context: open device / PD / port attrs / GID / MTU
 *  (QP is not created here — it is created after the topology is known)
 * ================================================================ */
static void setup_rdma_context(struct alltoall_context *ctx) {
    ctx->ib_dev = find_device(ctx->cfg.dev_name);
    ctx->context = ibv_open_device(ctx->ib_dev);
    if (!ctx->context) die("ibv_open_device failed");

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) die("ibv_alloc_pd failed");

    ctx->port_num = 1;

    if (ibv_query_device(ctx->context, &ctx->dev_attr))
        die("ibv_query_device failed");
    printf("Device caps: max_qp_wr=%d, max_cqe=%d, max_mr_size=0x%lx\n",
           ctx->dev_attr.max_qp_wr, ctx->dev_attr.max_cqe,
           ctx->dev_attr.max_mr_size);

    if (ibv_query_port(ctx->context, ctx->port_num, &ctx->port_attr))
        die("ibv_query_port failed");
    ctx->own_lid = ctx->port_attr.lid;

    /* print GID table to help locate the RoCEv2 GID index */
    printf("GID table for %s (port %d):\n", ctx->cfg.dev_name, ctx->port_num);
    for (int g = 0; g < 16; g++) {
        union ibv_gid gid;
        if (ibv_query_gid(ctx->context, ctx->port_num, g, &gid)) break;
        uint32_t *raw = (uint32_t *)gid.raw;
        if (raw[0] == 0 && raw[1] == 0 && ntohl(raw[2]) == 0x0000ffff) {
            struct in_addr ip;
            ip.s_addr = raw[3];
            printf("  %-5d  ::ffff:%s  <- RoCEv2\n", g, inet_ntoa(ip));
        } else if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0) {
            printf("  %-5d  (zeros - likely unused)\n", g);
        } else {
            printf("  %-5d  %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n", g,
                   ntohs(((uint16_t*)raw)[0]), ntohs(((uint16_t*)raw)[1]),
                   ntohs(((uint16_t*)raw)[2]), ntohs(((uint16_t*)raw)[3]),
                   ntohs(((uint16_t*)raw)[4]), ntohs(((uint16_t*)raw)[5]),
                   ntohs(((uint16_t*)raw)[6]), ntohs(((uint16_t*)raw)[7]));
        }
    }
    printf("  Using GID index %d\n", ctx->cfg.gid_index);

    /* query this node's GID / IP */
    if (ibv_query_gid(ctx->context, ctx->port_num, ctx->cfg.gid_index,
                      &ctx->own_gid))
        die("ibv_query_gid failed");

    {
        int zero = 1;
        for (int k = 0; k < 16; k++)
            if (ctx->own_gid.raw[k] != 0) { zero = 0; break; }
        if (zero) {
            fprintf(stderr, "ERROR: GID index %d for %s is all zeros - "
                    "not a valid RoCEv2 GID!\n",
                    ctx->cfg.gid_index, ctx->cfg.dev_name);
            exit(1);
        }
    }

    uint32_t *gid_raw = (uint32_t *)ctx->own_gid.raw;
    ctx->own_ip = gid_raw[3];   /* IPv4 s_addr */
    {
        struct in_addr ip;
        ip.s_addr = ctx->own_ip;
        printf("  My IP:     %s\n", inet_ntoa(ip));
    }

    printf("  Port state: %s\n  Link layer: %s\n",
           ctx->port_attr.state == IBV_PORT_ACTIVE ? "ACTIVE" :
           ctx->port_attr.state == IBV_PORT_DOWN   ? "DOWN" :
           ctx->port_attr.state == IBV_PORT_INIT   ? "INIT" : "UNKNOWN",
           ctx->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET ? "Ethernet" :
           ctx->port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" :
           "Unknown");

    /* MTU */
    if (ctx->cfg.mtu <= 0) {
        ctx->cfg.mtu = mtu_enum_to_bytes(ctx->port_attr.active_mtu);
        printf("Using port active MTU: %d bytes\n", ctx->cfg.mtu);
    } else {
        printf("Using user-specified MTU: %d bytes\n", ctx->cfg.mtu);
        if (ctx->cfg.mtu > mtu_enum_to_bytes(ctx->port_attr.active_mtu))
            printf("  WARNING: requested MTU exceeds port active MTU (%d)!\n",
                   mtu_enum_to_bytes(ctx->port_attr.active_mtu));
    }

    ctx->max_send_wr = 256;
    if (ctx->dev_attr.max_qp_wr < ctx->max_send_wr)
        ctx->max_send_wr = (int)ctx->dev_attr.max_qp_wr;
    printf("Per-QP max send WR: %d (device max: %d)\n",
           ctx->max_send_wr, ctx->dev_attr.max_qp_wr);

    srand((unsigned int)time(NULL));
}

/* ================================================================
 *  Allocate link array (K × num_qps); the rank==my_rank row is unused.
 *  Must be called after the topology and final num_qps are known.
 * ================================================================ */
static void alloc_links(struct alltoall_context *ctx) {
    int K  = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;

    ctx->links = (struct link_ctx **)calloc(K, sizeof(struct link_ctx *));
    if (!ctx->links) die("calloc links");
    for (int j = 0; j < K; j++) {
        ctx->links[j] = (struct link_ctx *)calloc(nq, sizeof(struct link_ctx));
        if (!ctx->links[j]) die("calloc links[j]");
    }
}

/* ================================================================
 *  Create resources for a single link (peer, qp): buffers / MR / CQ / QP
 * ================================================================ */
static void create_link_resources(struct alltoall_context *ctx, int peer, int q) {
    struct link_ctx *L = &ctx->links[peer][q];
    struct ibv_qp_init_attr qp_init_attr;
    int buf_size = ctx->cfg.msg_size * ctx->cfg.num_msgs;

    L->buf_size = buf_size;
    L->send_buf = (char *)calloc(1, buf_size);
    L->recv_buf = (char *)calloc(1, buf_size);
    if (!L->send_buf || !L->recv_buf) die("calloc buffer failed");

    /* fill send buffer pattern (for future validation); zero the recv buffer */
    for (int i = 0; i < buf_size; i++)
        L->send_buf[i] = (char)((i + q) & 0xFF);

    L->send_mr = ibv_reg_mr(ctx->pd, L->send_buf, buf_size, MR_ACCESS_FLAGS);
    L->recv_mr = ibv_reg_mr(ctx->pd, L->recv_buf, buf_size, MR_ACCESS_FLAGS);
    if (!L->send_mr || !L->recv_mr) die("ibv_reg_mr failed");

    L->cq = ibv_create_cq(ctx->context, 256, NULL, NULL, 0);
    if (!L->cq) die("ibv_create_cq failed");

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = L->cq;
    qp_init_attr.recv_cq = L->cq;
    qp_init_attr.cap.max_send_wr  = ctx->max_send_wr;
    qp_init_attr.cap.max_recv_wr  = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_RC;

    L->qp = ibv_create_qp(ctx->pd, &qp_init_attr);
    if (!L->qp) die("ibv_create_qp failed");

    L->local.lid    = ctx->own_lid;
    L->local.qp_num = L->qp->qp_num;
    L->local.psn    = (uint32_t)(rand() & 0xFFFFFF);
    L->local.gid    = ctx->own_gid;

    /* send-engine scratch */
    L->wrs  = (struct ibv_send_wr *)calloc(ctx->max_send_wr, sizeof(struct ibv_send_wr));
    L->sges = (struct ibv_sge *)calloc(ctx->max_send_wr, sizeof(struct ibv_sge));
    if (!L->wrs || !L->sges) die("calloc send scratch");

    modify_qp_to_init(L->qp, ctx->port_num);
}

/* ================================================================
 *  Structures exchanged over TCP (raw bytes; both sides are homogeneous x86_64 / same compiler)
 * ================================================================ */
struct link_info {                 /* connection/MR info of a node for a peer + a QP */
    uint16_t  lid;
    uint32_t  qp_num;
    uint32_t  psn;
    uint8_t   gid[16];
    uint64_t  buf_addr;            /* local recv buffer addr (peer WRITE target) */
    uint32_t  rkey;
    uint32_t  _pad;
};

struct reg_msg {                   /* client → server registration info */
    uint32_t my_ip;
    int      num_nodes;
    int      num_qps;
    int      msg_size;
    int      num_msgs;
};

struct config_msg {                /* server → client topology/config */
    int      num_nodes;
    int      my_rank;
    int      num_qps;
    int      msg_size;
    int      num_msgs;
    uint32_t node_ips[MAX_NODES];
};

struct result_msg {                /* client → server result */
    double   fct_us;
    uint64_t bytes_sent;
};

static void fill_link_info(struct link_info *li, struct link_ctx *L) {
    memset(li, 0, sizeof(*li));
    li->lid    = L->local.lid;
    li->qp_num = L->local.qp_num;
    li->psn    = L->local.psn;
    memcpy(li->gid, L->local.gid.raw, 16);
    li->buf_addr = (uint64_t)(uintptr_t)L->recv_buf;
    li->rkey     = L->recv_mr->rkey;
}

static void apply_link_info(struct link_ctx *L, const struct link_info *li) {
    L->remote.lid    = li->lid;
    L->remote.qp_num = li->qp_num;
    L->remote.psn    = li->psn;
    memcpy(L->remote.gid.raw, li->gid, 16);
    L->remote_addr = li->buf_addr;
    L->remote_rkey = li->rkey;
}

/* ================================================================
 *  TCP server: listen and accept K-1 clients, validate registration info
 * ================================================================ */
static void server_accept_clients(struct alltoall_context *ctx) {
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

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind() failed");
    if (listen(listen_fd, K) < 0)
        die("listen() failed");
    ctx->listen_fd = listen_fd;

    printf("[Server] TCP listening on 0.0.0.0:%d, waiting for %d client(s)...\n",
           ctx->cfg.tcp_port, K - 1);

    ctx->node_ips[0] = ctx->own_ip;   /* rank 0 = server */

    for (int r = 1; r < K; r++) {
        struct sockaddr_in caddr;
        socklen_t alen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &alen);
        if (cfd < 0) die("accept() failed");

        struct reg_msg reg;
        if (tcp_read_full(cfd, &reg, sizeof(reg))) die("tcp read reg");
        {
            struct in_addr rip;   /* client's real RDMA identity (from its RoCEv2 GID) */
            rip.s_addr = reg.my_ip;
            printf("[Server] Client #%d (rank %d): GID-IP=%s, TCP-src=%s | nodes=%d, QPs=%d, msg_size=%d, num_msgs=%d\n",
                   r, r, inet_ntoa(rip), inet_ntoa(caddr.sin_addr),
                   reg.num_nodes, reg.num_qps, reg.msg_size, reg.num_msgs);
        }

        /* validate consistency with server config */
        if (reg.num_nodes != ctx->cfg.num_nodes ||
            reg.num_qps != ctx->cfg.num_qps ||
            reg.msg_size != ctx->cfg.msg_size ||
            reg.num_msgs != ctx->cfg.num_msgs) {
            fprintf(stderr, "ERROR: client #%d config mismatch!\n", r);
            fprintf(stderr, "  server: N=%d n=%d s=%d m=%d\n  client: N=%d n=%d s=%d m=%d\n",
                    ctx->cfg.num_nodes, ctx->cfg.num_qps,
                    ctx->cfg.msg_size, ctx->cfg.num_msgs,
                    reg.num_nodes, reg.num_qps, reg.msg_size, reg.num_msgs);
            exit(1);
        }

        ctx->client_fds[r] = cfd;
        ctx->node_ips[r]   = reg.my_ip;
    }

    printf("[Server] All %d clients connected. Topology:\n", K - 1);
    for (int r = 0; r < K; r++) {
        struct in_addr ip;
        ip.s_addr = ctx->node_ips[r];
        printf("  rank %d: %s%s\n", r, inet_ntoa(ip),
               r == 0 ? "  (server)" : "");
    }
    printf("\n");
}

/* ================================================================
 *  TCP client: connect to server and register
 * ================================================================ */
static void client_connect(struct alltoall_context *ctx) {
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) die("socket() failed");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)ctx->cfg.tcp_port);
    if (inet_pton(AF_INET, ctx->cfg.server_ip, &addr.sin_addr) != 1)
        die("Invalid server IP");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("connect() failed");
    ctx->tcp_fd = sock;
    printf("[Client] TCP connected to %s:%d\n", ctx->cfg.server_ip, ctx->cfg.tcp_port);

    struct reg_msg reg;
    reg.my_ip     = ctx->own_ip;
    reg.num_nodes = ctx->cfg.num_nodes;
    reg.num_qps   = ctx->cfg.num_qps;
    reg.msg_size  = ctx->cfg.msg_size;
    reg.num_msgs  = ctx->cfg.num_msgs;
    if (tcp_write_full(sock, &reg, sizeof(reg))) die("tcp write reg");

    /* receive topology/config */
    struct config_msg cm;
    if (tcp_read_full(sock, &cm, sizeof(cm))) die("tcp read config");

    ctx->num_nodes = cm.num_nodes;
    ctx->my_rank   = cm.my_rank;
    ctx->cfg.num_qps  = cm.num_qps;   /* adopt server's authoritative config */
    ctx->cfg.msg_size = cm.msg_size;
    ctx->cfg.num_msgs = cm.num_msgs;
    memcpy(ctx->node_ips, cm.node_ips, MAX_NODES * sizeof(uint32_t));

    printf("[Client] My rank=%d, nodes=%d. Topology:\n", ctx->my_rank, ctx->num_nodes);
    for (int r = 0; r < ctx->num_nodes; r++) {
        struct in_addr ip;
        ip.s_addr = ctx->node_ips[r];
        printf("  rank %d: %s%s\n", r, inet_ntoa(ip), r == ctx->my_rank ? "  (me)" : "");
    }
    printf("\n");
}

/* ================================================================
 *  Centralized QP/MR info exchange (routed via the server)
 *
 *  Each node reports its local info report[me][peer][q] for every peer:
 *    {local qp_num/psn/lid/gid, local recv buffer addr/rkey}
 *  The server aggregates them, then sends report[p][me][q] (peer p's info toward me) to me.
 * ================================================================ */
static void exchange_link_info(struct alltoall_context *ctx) {
    int K = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;
    /* reports[from][to][q] */
    struct link_info *reports[MAX_NODES][MAX_NODES];
    for (int i = 0; i < K; i++)
        for (int j = 0; j < K; j++)
            reports[i][j] = (struct link_info *)calloc(nq, sizeof(struct link_info));

    if (ctx->cfg.is_server) {
        /* 1. server's own info */
        for (int j = 1; j < K; j++)
            for (int q = 0; q < nq; q++)
                fill_link_info(&reports[0][j][q], &ctx->links[j][q]);

        /* 2. receive each client's report (ascending peer rank, skip itself) */
        for (int r = 1; r < K; r++) {
            for (int p = 0; p < K; p++) {
                if (p == r) continue;
                if (tcp_read_full(ctx->client_fds[r], reports[r][p],
                                  nq * sizeof(struct link_info)))
                    die("tcp read report");
            }
        }

        /* 3. distribute to each client: report[p][r][q] to rank r */
        for (int r = 1; r < K; r++) {
            for (int p = 0; p < K; p++) {
                if (p == r) continue;
                if (tcp_write_full(ctx->client_fds[r], reports[p][r],
                                   nq * sizeof(struct link_info)))
                    die("tcp write routed info");
            }
        }

        /* 4. server applies: peer j's info = report[j][0][q] */
        for (int j = 1; j < K; j++)
            for (int q = 0; q < nq; q++)
                apply_link_info(&ctx->links[j][q], &reports[j][0][q]);
    } else {
        int me = ctx->my_rank;

        /* 1. report local info (ascending peer rank, skip itself) */
        for (int p = 0; p < K; p++) {
            if (p == me) continue;
            for (int q = 0; q < nq; q++)
                fill_link_info(&reports[me][p][q], &ctx->links[p][q]);
            if (tcp_write_full(ctx->tcp_fd, reports[me][p],
                               nq * sizeof(struct link_info)))
                die("tcp write report");
        }

        /* 2. receive routed peer info report[p][me][q] */
        for (int p = 0; p < K; p++) {
            if (p == me) continue;
            struct link_info *buf = (struct link_info *)calloc(nq, sizeof(struct link_info));
            if (tcp_read_full(ctx->tcp_fd, buf, nq * sizeof(struct link_info)))
                die("tcp read routed info");
            for (int q = 0; q < nq; q++)
                apply_link_info(&ctx->links[p][q], &buf[q]);
            free(buf);
        }
    }

    /* free temporary storage */
    for (int i = 0; i < K; i++)
        for (int j = 0; j < K; j++)
            free(reports[i][j]);
}

/* ================================================================
 *  Bring all QPs to RTR + RTS
 *  Two-phase: all RTR first, then all RTS, to avoid connection-setup races in the full mesh.
 * ================================================================ */
static void bring_qps_up(struct alltoall_context *ctx) {
    enum ibv_mtu mtu_enum = mtu_bytes_to_enum(ctx->cfg.mtu);
    int K = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;

    /* Phase 1: all QPs -> RTR */
    for (int j = 0; j < K; j++) {
        if (j == ctx->my_rank) continue;
        struct in_addr ip;
        ip.s_addr = ctx->node_ips[j];
        printf("[rank %d] RTR to rank %d (%s), %d QP(s)...\n",
               ctx->my_rank, j, inet_ntoa(ip), nq);
        for (int q = 0; q < nq; q++)
            modify_qp_to_rtr(ctx->links[j][q].qp, &ctx->links[j][q].remote,
                             ctx->port_num, ctx->cfg.gid_index, mtu_enum);
    }

    /* Phase 2: all QPs -> RTS */
    for (int j = 0; j < K; j++) {
        if (j == ctx->my_rank) continue;
        for (int q = 0; q < nq; q++)
            modify_qp_to_rts(ctx->links[j][q].qp, ctx->links[j][q].local.psn);
    }
    printf("[rank %d] All QPs -> RTS.\n", ctx->my_rank);
}

/* ================================================================
 *  Warmup: send one 8-byte RDMA WRITE per QP to prime the HW pipeline
 *  (same as rdma_example's warmup, but here all nodes send, so all warm up)
 * ================================================================ */
static void warmup(struct alltoall_context *ctx) {
    int K = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;

    printf("[rank %d] Warming up %d QP(s)...\n", ctx->my_rank, (K - 1) * nq);
    for (int j = 0; j < K; j++) {
        if (j == ctx->my_rank) continue;
        for (int q = 0; q < nq; q++) {
            struct link_ctx *L = &ctx->links[j][q];
            struct ibv_sge sge;
            struct ibv_send_wr wr, *bad_wr;
            struct ibv_wc wc;

            sge.addr   = (uintptr_t)L->send_buf;
            sge.length = 8;
            sge.lkey   = L->send_mr->lkey;

            memset(&wr, 0, sizeof(wr));
            wr.wr_id      = 0;
            wr.sg_list    = &sge;
            wr.num_sge    = 1;
            wr.opcode     = IBV_WR_RDMA_WRITE;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = L->remote_addr;
            wr.wr.rdma.rkey        = L->remote_rkey;

            if (ibv_post_send(L->qp, &wr, &bad_wr))
                die("warmup ibv_post_send failed");

            while (ibv_poll_cq(L->cq, 1, &wc) == 0)
                ;
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[rank %d] warmup QP(%d,%d) failed: %s\n",
                        ctx->my_rank, j, q, ibv_wc_status_str(wc.status));
                die("warmup: QP not functional - check device/switch/PFC");
            }
        }
    }
    printf("[rank %d] Warm-up complete.\n", ctx->my_rank);
}

/* ================================================================
 *  Post a batch of RDMA WRITEs (messages start_msg .. start_msg+bsize-1)
 * ================================================================ */
static void post_batch(struct link_ctx *L, int bsize, int msg_size, int start_msg) {
    for (int i = 0; i < bsize; i++) {
        int idx = start_msg + i;

        L->sges[i].addr   = (uintptr_t)(L->send_buf + (size_t)idx * msg_size);
        L->sges[i].length = (uint32_t)msg_size;
        L->sges[i].lkey   = L->send_mr->lkey;

        memset(&L->wrs[i], 0, sizeof(L->wrs[i]));
        L->wrs[i].wr_id      = (uint64_t)idx;
        L->wrs[i].sg_list    = &L->sges[i];
        L->wrs[i].num_sge    = 1;
        L->wrs[i].opcode     = IBV_WR_RDMA_WRITE;
        L->wrs[i].send_flags = (i == bsize - 1) ? IBV_SEND_SIGNALED : 0;
        L->wrs[i].wr.rdma.remote_addr = L->remote_addr + (size_t)idx * msg_size;
        L->wrs[i].wr.rdma.rkey        = L->remote_rkey;
        L->wrs[i].next = (i < bsize - 1) ? &L->wrs[i + 1] : NULL;
    }

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(L->qp, &L->wrs[0], &bad_wr))
        die("ibv_post_send failed in alltoall");
}

/* ================================================================
 *  Timed alltoall send: this node sends to all peers in parallel; returns local FCT (us)
 *
 *  Uses a polling batch engine; links/QPs interleave posts to keep the pipeline full.
 * ================================================================ */
static double run_timed_alltoall(struct alltoall_context *ctx) {
    int K = ctx->num_nodes;
    int nq = ctx->cfg.num_qps;
    int num_msgs = ctx->cfg.num_msgs;
    int msg_size = ctx->cfg.msg_size;
    int batch = ctx->max_send_wr;

    int msgs_per_qp = num_msgs / nq;
    int remainder   = num_msgs % nq;

    struct active_link {
        struct link_ctx *L;
        int peer;       /* peer global rank */
        int q;          /* QP index */
        int n_msgs;     /* msgs this link/QP must send */
        int sent;       /* msgs sent and completed */
        int inflight;   /* current in-flight batch size */
    };
    int nlinks = (K - 1) * nq;
    struct active_link *al = (struct active_link *)calloc(nlinks, sizeof(*al));

    int idx = 0;
    for (int j = 0; j < K; j++) {
        if (j == ctx->my_rank) continue;
        for (int q = 0; q < nq; q++) {
            al[idx].L       = &ctx->links[j][q];
            al[idx].peer    = j;
            al[idx].q       = q;
            al[idx].n_msgs  = msgs_per_qp + (q < remainder ? 1 : 0);
            al[idx].sent    = 0;
            al[idx].inflight = 0;
            idx++;
        }
    }

    printf("[rank %d] Starting timed alltoall: %d peers x %d QPs, %d msgs/peer, %.2f KiB/msg\n",
           ctx->my_rank, K - 1, nq, num_msgs, msg_size / 1024.0);

    double start = get_time_us();

    /* post first batch */
    int active = 0;
    for (int i = 0; i < nlinks; i++) {
        if (al[i].n_msgs <= 0) continue;
        int bsize = MIN(batch, al[i].n_msgs);
        post_batch(al[i].L, bsize, msg_size, al[i].sent);
        al[i].inflight = bsize;
        active++;
    }

    /* poll until all links complete */
    while (active > 0) {
        for (int i = 0; i < nlinks; i++) {
            if (al[i].inflight == 0) continue;
            struct ibv_wc wc;
            int ne = ibv_poll_cq(al[i].L->cq, 1, &wc);
            if (ne < 0) die("poll_cq error during alltoall");
            if (ne == 0) continue;

            if (wc.status != IBV_WC_SUCCESS) {
                struct in_addr ip;
                ip.s_addr = ctx->node_ips[al[i].peer];
                fprintf(stderr, "[rank %d] WC error on QP -> peer rank %d (%s): %s\n",
                        ctx->my_rank, al[i].peer, inet_ntoa(ip),
                        ibv_wc_status_str(wc.status));
                exit(EXIT_FAILURE);
            }

            al[i].sent += al[i].inflight;
            if (al[i].sent < al[i].n_msgs) {
                int bsize = MIN(batch, al[i].n_msgs - al[i].sent);
                post_batch(al[i].L, bsize, msg_size, al[i].sent);
                al[i].inflight = bsize;
            } else {
                al[i].inflight = 0;
                active--;
            }
        }
    }

    double end = get_time_us();
    double fct = end - start;
    free(al);
    return fct;
}

/* ================================================================
 *  Cleanup resources
 * ================================================================ */
static void cleanup(struct alltoall_context *ctx) {
    if (ctx->links) {
        for (int j = 0; j < ctx->num_nodes; j++) {
            for (int q = 0; q < ctx->cfg.num_qps; q++) {
                struct link_ctx *L = &ctx->links[j][q];
                if (L->qp)      ibv_destroy_qp(L->qp);
                if (L->cq)      ibv_destroy_cq(L->cq);
                if (L->send_mr) ibv_dereg_mr(L->send_mr);
                if (L->recv_mr) ibv_dereg_mr(L->recv_mr);
                free(L->send_buf);
                free(L->recv_buf);
                free(L->wrs);
                free(L->sges);
            }
            free(ctx->links[j]);
        }
        free(ctx->links);
    }
    if (ctx->pd)      ibv_dealloc_pd(ctx->pd);
    if (ctx->context) ibv_close_device(ctx->context);
}

static void sig_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nInterrupted. Cleaning up...\n");
    if (g_ctx) {
        if (g_ctx->listen_fd > 0) close(g_ctx->listen_fd);
        if (g_ctx->tcp_fd > 0)    close(g_ctx->tcp_fd);
        for (int r = 0; r < MAX_NODES; r++)
            if (g_ctx->client_fds[r] > 0) close(g_ctx->client_fds[r]);
        cleanup(g_ctx);
    }
    exit(0);
}

/* ================================================================
 *  main
 * ================================================================ */
int main(int argc, char *argv[]) {
    struct alltoall_context ctx;
    double fct_us = 0.0;

    memset(&ctx, 0, sizeof(ctx));
    parse_args(argc, argv, &ctx.cfg);
    g_ctx = &ctx;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        RoCEv2 All-to-All Traffic Generator              ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    print_config(&ctx.cfg);

    /* 1. RDMA context (no QP yet) */
    setup_rdma_context(&ctx);

    /* 2. build topology / exchange config */
    if (ctx.cfg.is_server) {
        ctx.num_nodes = ctx.cfg.num_nodes;
        server_accept_clients(&ctx);
        ctx.my_rank = 0;

        /* broadcast config/topology to each client */
        struct config_msg cm;
        cm.num_nodes = ctx.num_nodes;
        cm.num_qps   = ctx.cfg.num_qps;
        cm.msg_size  = ctx.cfg.msg_size;
        cm.num_msgs  = ctx.cfg.num_msgs;
        memcpy(cm.node_ips, ctx.node_ips, MAX_NODES * sizeof(uint32_t));
        for (int r = 1; r < ctx.num_nodes; r++) {
            cm.my_rank = r;
            if (tcp_write_full(ctx.client_fds[r], &cm, sizeof(cm)))
                die("tcp write config");
        }
    } else {
        client_connect(&ctx);   /* includes register + receive topology */
    }

    /* 3. allocate link array (topology/num_qps known) */
    alloc_links(&ctx);

    /* 4. create QPs for all links */
    for (int j = 0; j < ctx.num_nodes; j++) {
        if (j == ctx.my_rank) continue;
        for (int q = 0; q < ctx.cfg.num_qps; q++)
            create_link_resources(&ctx, j, q);
    }

    /* 5. exchange QP/MR info */
    exchange_link_info(&ctx);

    /* 6. QPs to RTR/RTS */
    bring_qps_up(&ctx);

    /* 7. warmup */
    warmup(&ctx);

    /* 8. barrier + timed alltoall */
    if (ctx.cfg.is_server) {
        int K = ctx.num_nodes;
        /* wait for all clients READY */
        for (int r = 1; r < K; r++) {
            int ready = 0;
            if (tcp_read_full(ctx.client_fds[r], &ready, sizeof(ready)))
                die("tcp read ready");
        }
        printf("[Server] All clients ready. Broadcasting GO...\n");
        int go = 1;
        for (int r = 1; r < K; r++)
            if (tcp_write_full(ctx.client_fds[r], &go, sizeof(go)))
                die("tcp write go");

        fct_us = run_timed_alltoall(&ctx);

        /* collect each client's result */
        struct result_msg results[MAX_NODES];
        results[0].fct_us = fct_us;
        results[0].bytes_sent = (uint64_t)ctx.cfg.num_msgs *
                                 (uint64_t)ctx.cfg.msg_size * (uint64_t)(K - 1);
        for (int r = 1; r < K; r++) {
            if (tcp_read_full(ctx.client_fds[r], &results[r], sizeof(struct result_msg)))
                die("tcp read result");
        }

        /* summarize */
        double max_fct = 0.0;
        uint64_t total_bytes = 0;
        for (int r = 0; r < K; r++) {
            if (results[r].fct_us > max_fct) max_fct = results[r].fct_us;
            total_bytes += results[r].bytes_sent;
        }
        double agg_bw_gbps = (double)total_bytes * 8.0 / 1e9 / (max_fct / 1e6);

        printf("\n╔════════════════════════════════════════════════════════════════╗\n");
        printf("║           All-to-All Results (coordinator view)              ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║  rank  %-15s  %12s  %12s  %14s ║\n",
               "IP", "Bytes Sent", "FCT (us)", "Node BW (Gbps)");
        printf("╠────────────────────────────────────────────────────────────────╣\n");
        for (int r = 0; r < K; r++) {
            struct in_addr ip;
            ip.s_addr = ctx.node_ips[r];
            double nbw = (double)results[r].bytes_sent * 8.0 / 1e9 /
                         (results[r].fct_us / 1e6);
            printf("║  %4d  %-15s  %12lu  %12.2f  %14.2f ║\n",
                   r, inet_ntoa(ip),
                   results[r].bytes_sent, results[r].fct_us, nbw);
        }
        printf("╠────────────────────────────────────────────────────────────────╣\n");
        printf("║  Aggregate traffic:   %.2f MiB                               ║\n",
               (double)total_bytes / (1024.0 * 1024.0));
        printf("║  Alltoall FCT (max):  %.2f us  (%.3f ms)                     ║\n",
               max_fct, max_fct / 1000.0);
        printf("║  Aggregate bandwidth: %.2f Gbps                              ║\n",
               agg_bw_gbps);
        printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    } else {
        int ready = 1;
        if (tcp_write_full(ctx.tcp_fd, &ready, sizeof(ready))) die("tcp write ready");
        int go = 0;
        if (tcp_read_full(ctx.tcp_fd, &go, sizeof(go))) die("tcp read go");

        fct_us = run_timed_alltoall(&ctx);

        struct result_msg res;
        res.fct_us = fct_us;
        res.bytes_sent = (uint64_t)ctx.cfg.num_msgs *
                         (uint64_t)ctx.cfg.msg_size *
                         (uint64_t)(ctx.num_nodes - 1);
        if (tcp_write_full(ctx.tcp_fd, &res, sizeof(res))) die("tcp write result");

        double nbw = (double)res.bytes_sent * 8.0 / 1e9 / (fct_us / 1e6);
        printf("\n[Client rank %d] Alltoall complete:\n", ctx.my_rank);
        printf("  Bytes sent:  %.2f MiB\n", (double)res.bytes_sent / (1024.0 * 1024.0));
        printf("  FCT:         %.2f us  (%.3f ms)\n", fct_us, fct_us / 1000.0);
        printf("  Node BW:     %.2f Gbps\n", nbw);
    }

    /* 9. cleanup */
    if (ctx.cfg.is_server) {
        for (int r = 1; r < ctx.num_nodes; r++)
            if (ctx.client_fds[r] > 0) close(ctx.client_fds[r]);
        if (ctx.listen_fd > 0) close(ctx.listen_fd);
    } else {
        if (ctx.tcp_fd > 0) close(ctx.tcp_fd);
    }
    cleanup(&ctx);

    printf("[rank %d] Done.\n", ctx.my_rank);
    return 0;
}
