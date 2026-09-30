# RoCEv2 Ring AllReduce Makefile
# Deps: libibverbs-dev, librdmacm-dev (Ubuntu/Debian)
#       libibverbs-devel, librdmacm-devel (RHEL/CentOS)

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g
LDFLAGS  = -libverbs -lrdmacm

TARGET   = rdma_allreduce

.PHONY: all clean install-deps install-deps-rhel help

all: $(TARGET)

$(TARGET): rdma_allreduce.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install-deps:
	sudo apt-get update
	sudo apt-get install -y libibverbs-dev librdmacm-dev rdma-core ibverbs-utils perftest

install-deps-rhel:
	sudo yum install -y libibverbs-devel librdmacm-devel rdma-core libibverbs-utils perftest

help:
	@echo "=== RoCEv2 Ring AllReduce ==="
	@echo ""
	@echo "Build:"
	@echo "  make               build rdma_allreduce"
	@echo "  make clean         clean"
	@echo ""
	@echo "Options (same as rdma_example / rdma_alltoall):"
	@echo "  -d  device       (default: mlx5_0)"
	@echo "  -g  GID index    (default: 3)"
	@echo "  -n  QPs (per ring direction) (default: 1)"
	@echo "  -m  msgs (per node)   (default: 100)"
	@echo "  -s  msg size (byte)   (default: 1024000 ~= 1 MiB)"
	@echo "  -t  MTU(byte)    (default: from ibv_devinfo)"
	@echo "  -p  TCP port     (default: 19876)"
	@echo "  -N  node count   (default: 4, max: 16)"
	@echo "  -S  server mode (rank 0)"
	@echo "  -C  client mode (with -i)"
	@echo "  -i  server IP   (client only)"
	@echo ""
	@echo "Run example (4 ports: 10.1.1.1/10.1.1.2=host1, 10.1.1.3/10.1.1.4=host2):"
	@echo "  # host2, server (10.1.1.3):"
	@echo "  ./rdma_allreduce -S -d mlx5_0 -g 3 -N 4"
	@echo "  # host1 client 1 (10.1.1.1):"
	@echo "  ./rdma_allreduce -C -d mlx5_0 -i 10.1.1.3 -g 3 -N 4"
	@echo "  # host1 client 2 (10.1.1.2):"
	@echo "  ./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4"
	@echo "  # host2 client 3 (10.1.1.4):"
	@echo "  ./rdma_allreduce -C -d mlx5_1 -i 10.1.1.3 -g 3 -N 4"
	@echo ""
	@echo "Note: num_msgs*msg_size must be divisible by the node count, and each chunk by the QP count (uint64 reduction)."
