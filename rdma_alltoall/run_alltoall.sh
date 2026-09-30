#!/bin/bash
# run_alltoall.sh — All-to-All traffic generator launcher script
#
# Build first: make
#
# 4-port topology (host1: 10.1.1.1=mlx5_0, 10.1.1.2=mlx5_1; host2: 10.1.1.3=mlx5_0, 10.1.1.4=mlx5_1)
#
# Start order: start the server (any port) first, then the remaining clients.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${SCRIPT_DIR}/rdma_alltoall"

[ -x "$BIN" ] || { echo "Build first: make"; exit 1; }

ROLE="${1:-help}"
shift 2>/dev/null || true

case "$ROLE" in
    server)
        exec "$BIN" -S "$@"
        ;;
    client)
        exec "$BIN" -C "$@"
        ;;
    # ---- Preset: host2:10.1.1.3 as server ----
    h2-mlx5_0)   exec "$BIN" -S -d mlx5_0 -g 3 -N 4 "$@" ;;
    h2-mlx5_1)   exec "$BIN" -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 "$@" ;;  # host2 fourth node (client)
    h1-mlx5_0)   exec "$BIN" -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 "$@" ;;  # host1 client 1
    h1-mlx5_1)   exec "$BIN" -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4 "$@" ;;  # host1 client 2

    help|*)
        echo "Usage: $0 <server|client|preset> [extra args...]"
        echo ""
        echo "=== Roles ==="
        echo "  server -d <dev> ...        start server (coordinator, rank 0)"
        echo "  client -d <dev> -i <ip> ... start client"
        echo ""
        echo "=== Presets (10.1.1.3 as server, 4-node full mesh) ==="
        echo "  h2-mlx5_0   host2 server (10.1.1.3)"
        echo "  h2-mlx5_1   host2 client (10.1.1.4)"
        echo "  h1-mlx5_0   host1 client (10.1.1.1)"
        echo "  h1-mlx5_1   host1 client (10.1.1.2)"
        echo ""
        echo "=== Typical workflow ==="
        echo "  # start the server on host2 first:"
        echo "  $0 h2-mlx5_0"
        echo "  # start the fourth node on host2 (another terminal):"
        echo "  $0 h2-mlx5_1"
        echo "  # start two clients on host1 (two terminals):"
        echo "  $0 h1-mlx5_0"
        echo "  $0 h1-mlx5_1"
        echo ""
        echo "=== Custom args (same as rdma_example) ==="
        echo "  $0 server -d mlx5_0 -g 3 -N 4 -n 2 -m 500 -s 65536 -t 4096"
        echo "  $0 client -d mlx5_0 -i 10.1.1.3 -g 3 -N 4 -n 2 -m 500 -s 65536"
        ;;
esac
