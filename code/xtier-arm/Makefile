# SPDX-License-Identifier: GPL-2.0
CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wextra -g
MODEL_HDR ?= ml/models/mlp_q8_bootstrap.h
INC     = -I src -DXTIER_MODEL_HDR='"$(abspath $(MODEL_HDR))"'

BIN = src/xtierd
BENCH = bench/zipf_chase bench/phase_zipf

all: $(BIN) $(BENCH)

src/xtierd: src/xtierd.c src/spe.c src/policy.c src/model_if.c src/exec.c src/fault_ch.c src/xtierd.h src/model_if.h $(MODEL_HDR) src/fault_kern.o
	$(CC) $(CFLAGS) $(INC) -o $@ src/xtierd.c src/spe.c src/policy.c src/model_if.c src/exec.c src/fault_ch.c -lbpf -lelf -lz -lm

src/fault_kern.o: bpf/fault_kern.c
	LLVM ?= /opt/homebrew/opt/llvm/bin
	$(LLVM)/clang -target bpfel -O2 -g -Wall -Werror -c $< -o $@

ml/models/mlp_q8_bootstrap.h: ml/bootstrap_model.py
	python3 ml/bootstrap_model.py $@

bench/zipf_chase: bench/zipf_chase.c
	$(CC) -O2 -fopenmp -o $@ $< -lm
bench/phase_zipf: bench/phase_zipf.c
	$(CC) -O2 -fopenmp -o $@ $< -lm

clean:
	rm -f $(BIN) $(BENCH)

.PHONY: all clean
