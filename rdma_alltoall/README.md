# rdma_alltoall — RoCEv2 All-to-All Traffic Generator

Builds a **full-mesh** of RC QP connections among N RDMA ports (nodes). Each node performs
RDMA WRITE to the other N-1 nodes, generating an alltoall traffic pattern and measuring each
node's **flow completion time (FCT)** and the **aggregate bandwidth**.

Keeps the same parameter interface as `../rdma_example` (device / GID / #QPs / msg size /
#msgs / MTU), and also includes the **warmup** step.

## Relationship to rdma_example

| Dimension | rdma_example | rdma_alltoall |
|-----------|--------------|---------------|
| Topology  | one-to-one (client → server) | N-node full mesh |
| Traffic   | one-way batched WRITE | alltoall (each node → all others) |
| `-n`      | QPs on a single connection | QPs per link (per node pair) |
| `-m`      | total messages | msgs each node sends to **each peer** |
| `-s`      | msg size | msg size (same) |
| `-g`      | GID index | GID index (same) |
| `-t`      | MTU | MTU (same) |
| warmup    | client: one 8B WRITE per QP | **each node**: one 8B WRITE per QP |

## Build

On the RDMA hosts (host1/host2, with Mellanox NICs and libibverbs):

```bash
make
```

Dependencies (same as rdma_example): `libibverbs-dev` / `librdmacm-dev`.

## Topology and roles

This repo assumes 4 ports:

| host  | device | IP       |
|-------|--------|----------|
| host1 | mlx5_0 | 10.1.1.1 |
| host1 | mlx5_1 | 10.1.1.2 |
| host2 | mlx5_0 | 10.1.1.3 |
| host2 | mlx5_1 | 10.1.1.4 |

- **Server (rank 0, coordinator) is arbitrary**: whichever port runs `-S` becomes rank 0.
- **Clients = the remaining ports**: the other K-1 nodes join with `-C -i <server_ip>`.
- A node's identity (IP) is auto-resolved from its NIC's RoCEv2 GID; no need to pass its own IP.

## Run

Using 10.1.1.3 (host2/mlx5_0) as server, 4-node full mesh:

```bash
# 1) host2 — server (start first, wait for clients)
./rdma_alltoall -S -d mlx5_0 -g 3 -N 4 -n 1 -m 100 -s 1048576

# 2) host2 — fourth node (client, 10.1.1.4)
./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576

# 3) host1 — client 1 (10.1.1.1)
./rdma_alltoall -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576

# 4) host1 — client 2 (10.1.1.2)
./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576
```

Or use the script presets (see `run_alltoall.sh`):

```bash
# on host2
./run_alltoall.sh h2-mlx5_0          # server
./run_alltoall.sh h2-mlx5_1          # client 10.1.1.4
# on host1
./run_alltoall.sh h1-mlx5_0          # client 10.1.1.1
./run_alltoall.sh h1-mlx5_1          # client 10.1.1.2
```

> Note: all nodes must use **consistent** `-n` / `-m` / `-s` (the server validates and rejects
> inconsistent clients); `-N` must equal the actual number of nodes started.

## Command-line options

```
-S  server mode (coordinator, rank 0)
-C  client mode (with -i)
-i <ip>   server IP (client only)
-d <dev>  device          (default mlx5_0)
-g <idx>  GID index       (default 3)
-n <n>    QPs per link    (default 1)
-m <n>    msgs per peer   (default 100)
-s <b>    msg size        (default 1024000 ~= 1 MiB)
-t <b>    MTU             (default from ibv_devinfo)
-p <p>    TCP port        (default 19875)
-N <k>    alltoall nodes  (default 4, max 16)
```

## Execution flow

1. Each node opens the RDMA device and resolves its own GID / IP.
2. The server listens on TCP, accepts K-1 clients, validates config, and broadcasts topology + rank.
3. Each node creates `num_qps` RC QPs toward each of the other K-1 nodes (full mesh).
4. QP/MR info for every node pair is exchanged via **centralized routing** through the server; all QPs go RTR → RTS.
5. **Warmup**: each node sends one 8-byte RDMA WRITE per QP (first packet primes the HW pipeline).
6. Clients send READY, the server broadcasts the **GO** barrier, and all nodes start timing simultaneously.
7. Each node polls its CQs until all its WRITEs complete, then reports FCT to the server.
8. The server aggregates: per-node FCT/bandwidth, overall alltoall FCT (max over nodes), aggregate bandwidth.

## Metrics

- **Per-node FCT**: time from GO until that node's WRITEs are all acknowledged.
- **alltoall FCT**: the **max** of all nodes' FCT (the completion time of the whole alltoall).
- **Per-node bandwidth** = bytes sent by that node × 8 / its FCT.
- **Aggregate bandwidth** = total bytes sent by all nodes × 8 / alltoall FCT.

## Notes

- The **GO** broadcast from the server to clients has a small control-plane skew (much smaller than the data transfer time).
- This is a **traffic generation/measurement** tool; it does not verify data integrity (same as rdma_example).
- Memory usage: per node ≈ `2 × (K-1) × num_qps × num_msgs × msg_size` bytes (send + recv buffers).
- An RDMA WRITE sender-side completion means the data has been written to the peer's memory (RC semantics), so FCT is measured on the sender side.
- Ensure the switch has PFC/ECN lossless network config enabled (same prerequisite as rdma_example).


./rdma_alltoall -S -d mlx5_0 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_alltoall -C -d mlx5_0 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024

Note the order: 10.1.1.3 -> 10.1.1.4 -> 10.1.1.1 -> 10.1.1.2
./rdma_allreduce -S -d mlx5_0 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_allreduce -C -d mlx5_0 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024
./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -N 4 -n 1 -m 100 -s 409600 -t 1024
