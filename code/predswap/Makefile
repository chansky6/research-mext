# predswap Makefile:本地构建(BPF + Rust)与远程编译/上传
# 依赖:brew llvm、rustup target aarch64-unknown-linux-musl、llvm-tools(rust-lld)
HOST ?= root@192.168.90.7
REMOTE_DIR ?= /opt/predswap
LLVM ?= /opt/homebrew/opt/llvm/bin
SSH ?= ssh
SCP ?= scp

.PHONY: all bpf rust upload machine test

all: bpf rust

bpf:
	mkdir -p build
	$(LLVM)/clang -target bpfel -O2 -g -Wall -Werror -c bpf/predswap.bpf.c -o build/predswap.bpf.o

rust:
	cd daemon && cargo build --release --target aarch64-unknown-linux-musl

# 上传源码与本地构建产物,远程编译 C 部分(agent/bridge/bench/offprobe)
upload: all
	$(SSH) $(HOST) 'mkdir -p $(REMOTE_DIR)'
	$(SCP) -q bridge/ps_bridge.c agent/predswap_agent.c bench/synth.c bpf/offprobe.c \
	    build/predswap.bpf.o daemon/target/aarch64-unknown-linux-musl/release/predswapd $(HOST):$(REMOTE_DIR)/
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && \
		gcc -O2 -Wall -o ps_bridge ps_bridge.c -lbpf -lelf -lz && \
		gcc -O2 -Wall -fPIC -shared -o predswap_agent.so predswap_agent.c -lpthread && \
		gcc -O2 -Wall -o synth synth.c'

# BPF 结构偏移核对(换内核必跑;输出应与 bpf/predswap.bpf.c 的 VMF_ADDR_OFF 一致)
machine:
	$(SSH) $(HOST) 'K=/usr/src/kernels/$(shell $(SSH) $(HOST) uname -r 2>/dev/null || echo 5.10.0-216.0.0.115.oe2203sp4.aarch64); \
		gcc -O1 -w -o /tmp/offprobe -I$$K/include -I$$K/arch/arm64/include \
		-I$$K/arch/arm64/include/generated -I$$K/include/generated \
		-include $$K/include/linux/kconfig.h -D__KERNEL__ $(REMOTE_DIR)/offprobe.c && /tmp/offprobe'

test: upload
	$(SCP) -q bench/p1_validate.sh bench/fake_daemon.py bench/b1b2.sh $(HOST):/tmp/
