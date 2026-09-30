# rdma_allreduce — RoCEv2 Ring AllReduce

Forms a **ring** of K RDMA ports and runs the classic **ring allreduce** (Scatter-Reduce +
AllGather), summing each node's data block `S = num_msgs × msg_size` as uint64 and broadcasting
the result to all nodes. Includes result verification (`Verify OK`).

Keeps the same parameter interface and run style as `../rdma_example` / `../rdma_alltoall`.

## Topology and roles

- **Ring**: rank r's right neighbor `(r+1)%K`, left neighbor `(r-1+K)%K`. Data flows **one-way**
  (everyone sends to the right, receives from the left).
- **Server (rank 0) is arbitrary**: whichever port runs `-S` becomes rank 0.
- **Clients = the remaining K-1 ports**, joining with `-C -i <server_ip>`.
- A node's identity is auto-resolved from its RoCEv2 GID.

## Algorithm (Baidu classic ring allreduce)

Let each node's data be S, split into K chunks of `C = S/K` each.

1. **Scatter-Reduce (K-1 steps)**: at step s, node r sends chunk `(r-s) mod K` to the right and
   receives chunk `(r-s-1) mod K` from the left, **adding** it locally. Afterward node r holds the
   full sum of chunk `(r+1) mod K`.
2. **AllGather (K-1 steps)**: at step s, node r sends chunk `(r+1-s) mod K` to the right and
   receives chunk `(r-s) mod K` from the left, **overwriting** it. Afterward all nodes hold the sum
   of all K chunks.

## Synchronization

- Each ring direction has `num_qps` RC QPs; each chunk C is split into `num_qps` slices, one per QP.
- Each QP, after sending its slice, **sends an 8-byte flag (value 1)** (ordered within the same QP,
  guaranteeing data arrives first).
- The receiver polls its `num_qps` flags until all are set, then reduces/overwrites, then clears the flags.
- Each node warms up each out QP (8-byte RDMA WRITE).

## Build and run

On the RDMA hosts: `make`. Dependencies same as `rdma_example`.

Using 10.1.1.3 as server, 4 nodes:

```bash
# host2 server (10.1.1.3)
./rdma_allreduce -S -d mlx5_0 -g 3 -N 4 -n 1 -m 100 -s 1048576

# the other three clients
./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576   # host2 10.1.1.4
./rdma_allreduce -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576   # host1 10.1.1.1
./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 -n 1 -m 100 -s 1048576   # host1 10.1.1.2
```

## Command-line options (same as rdma_example, plus -N/-S/-C)

```
-S/-C/-i  role
-d <dev>  device           (default mlx5_0)
-g <idx>  GID index        (default 3)
-n <n>    QPs per ring direction (default 1)
-m <n>    msgs per node    (default 100)
-s <b>    msg size         (default 1024000)
-t <b>    MTU              (default from ibv_devinfo)
-p <p>    TCP port         (default 19876)
-N <k>    node count       (default 4, max 16)
```

## Constraints

- `msg_size` must be a multiple of 8 (uint64 word-wise reduction).
- `num_msgs × msg_size` must be divisible by the node count K (equal K chunks).
- Each chunk `S/K` must be divisible by the number of QPs.

## Metrics

- **Bytes sent per node** = `2 × (K-1) × C` (two phases of K-1 steps each, one chunk C per step).
- **Per-node FCT** = from GO until that node finishes both phases.
- **Allreduce FCT (max)** = max FCT over all nodes.
- **Aggregate bandwidth** = total bytes sent by all nodes × 8 / max(FCT).
- **Effective (S/FCT)** = useful data S × 8 / max(FCT), i.e. the "useful" reduction throughput.

## Relationship to alltoall

The relationship between aggregate and per-node bandwidth is the same: **aggregate bandwidth = K × slowest node's bandwidth**.

## Notes

- Lossless network (PFC) must be enabled end-to-end; otherwise large messages / heavy load will hit `transport retry counter exceeded` (see alltoall troubleshooting).
- If two ports on the same host cannot reach each other, static ARP or switch hairpin is needed (see alltoall troubleshooting).
- This tool does uint64 sum reduction and verifies `data_buf[w] == K(K+1)/2 × (w+1)`; data is filled per-rank so the check is meaningful.
