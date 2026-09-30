# RoCEv2 All-to-All Traffic Generator Makefile
# Deps: libibverbs-dev, librdmacm-dev (Ubuntu/Debian)
#       libibverbs-devel, librdmacm-devel (RHEL/CentOS)

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g
LDFLAGS  = -libverbs -lrdmacm

TARGET   = rdma_alltoall

.PHONY: all clean install-deps install-deps-rhel help

all: $(TARGET)

$(TARGET): rdma_alltoall.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install-deps:
	sudo apt-get update
	sudo apt-get install -y libibverbs-dev librdmacm-dev rdma-core ibverbs-utils perftest

install-deps-rhel:
	sudo yum install -y libibverbs-devel librdmacm-devel rdma-core libibverbs-utils perftest

help:
	@echo "=== RoCEv2 All-to-All Traffic Generator ==="
	@echo ""
	@echo "Build:"
	@echo "  make               build rdma_alltoall"
	@echo "  make clean         clean"
	@echo ""
	@echo "Options (same as rdma_example):"
	@echo "  -d  device       (default: mlx5_0)"
	@echo "  -g  GID index    (default: 3)"
	@echo "  -n  QPs (per link) (default: 1)"
	@echo "  -m  msgs (per peer) (default: 100)"
	@echo "  -s  msg size (byte) (default: 1024000 ~= 1 MiB)"
	@echo "  -t  MTU(byte)    (default: from ibv_devinfo)"
	@echo "  -p  TCP port     (default: 19875)"
	@echo "  -N  alltoall nodes (default: 4, max: 16)"
	@echo "  -S  server mode (coordinator rank 0)"
	@echo "  -C  client mode (with -i)"
	@echo "  -i  server IP   (client only)"
	@echo ""
	@echo "Run example (4 ports: 10.1.1.1/10.1.1.2=host1, 10.1.1.3/10.1.1.4=host2):"
	@echo "  # host2, server (arbitrary, e.g. 10.1.1.3 on mlx5_0):"
	@echo "  ./rdma_alltoall -S -d mlx5_0 -g 3 -N 4"
	@echo "  # host1, client 1 (10.1.1.1):"
	@echo "  ./rdma_alltoall -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4"
	@echo "  # host1, client 2 (10.1.1.2):"
	@echo "  ./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4"
	@echo "  # host2, client 3 (10.1.1.4):"
	@echo "  ./rdma_alltoall -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4"
