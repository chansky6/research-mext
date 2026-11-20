# predswap

MEXT 式预测性内存系统的自研实现(对标 AMD MEXT,见根目录《predswap-设计规格.md》v0.2 与《predswap-phase0-探测报告.md》)。

当前版本：M0–M4 在首次换入缺页出现时预测，在分类器确定 Hit/Major/Late 后记账。M4 后台训练 GRU，默认仅派发经独立窗口验证的短序列规则；agent 在被托管进程内执行 WILLNEED。模型记功不等于应用提速，实测及负结果见规格附录 C。

## 布局

| 目录 | 内容 | 构建方式 |
|---|---|---|
| `bpf/predswap.bpf.c` | eBPF:do_swap_page + swapin_readahead 双 entry kprobe,ringbuf 事件流 | 本地 `clang -target bpfel`(需要 `-g` 生成 BTF 供 libbpf 解析 .maps) |
| `bpf/offprobe.c` | `offsetof(struct vm_fault, address)` 探针(换内核必跑,结果写死进 .bpf.c 的 `VMF_ADDR_OFF`) | 目标机 gcc + kernel-devel 头 |
| `bridge/ps_bridge.c` | libbpf 加载器:事件流→stdout(32B/条),stdin 行协议 `+pid/-pid/quit` | 目标机 gcc `-lbpf -lelf -lz`(需 libbpf-devel;RLIMIT_MEMLOCK 必须 unlimited) |
| `agent/predswap_agent.c` | LD_PRELOAD 侧车:进程内 `madvise(MADV_WILLNEED)`(5.10 跨进程 WILLNEED 不可用,Phase 0 §2-2) | 目标机 gcc |
| `daemon/` | Rust 守护进程(musl 静态交叉编译):ingest/分类/状态/page_idle/卸载控制器/ctl/record | 本地 `cargo build --release --target aarch64-unknown-linux-musl`(linker=rust-lld,见 daemon/.cargo/config.toml,须在 daemon/ 目录内构建) |
| `bench/synth.c` | 合成负载:zipf/seq/chain/block/touch | 目标机 gcc |
| `bench/*.sh` | P1 验证与 B1/B2 对照脚本 | — |

## 构建

本地(mac arm64,需 brew llvm + rustup musl target + llvm-tools 组件):

```sh
cd bpf && clang -target bpfel -O2 -g -c predswap.bpf.c -o ../build/predswap.bpf.o
cd ../daemon && cargo build --release --target aarch64-unknown-linux-musl
```

目标机(一次性):

```sh
yum install -y libbpf-devel elfutils-libelf-devel kernel-devel-$(uname -r)
gcc -O2 -o ps_bridge bridge/ps_bridge.c -lbpf -lelf -lz
gcc -O2 -fPIC -shared -o predswap_agent.so agent/predswap_agent.c -lpthread
gcc -O2 -o synth bench/synth.c
# BPF 结构偏移(换内核后必跑):
gcc -O1 -w -o /tmp/offprobe -I/usr/src/kernels/$(uname -r)/include \
    -I/usr/src/kernels/$(uname -r)/arch/arm64/include \
    -I/usr/src/kernels/$(uname -r)/arch/arm64/include/generated \
    -I/usr/src/kernels/$(uname -r)/include/generated \
    -include /usr/src/kernels/$(uname -r)/include/linux/kconfig.h -D__KERNEL__ bpf/offprobe.c
/tmp/offprobe  # 输出 VMF_ADDR_OFF,若与本机 .bpf.c 不符需改后重新编译 BPF
```

## 运行(B2)

```sh
mkdir -p /sys/fs/cgroup/memory/bench0
echo 100 > /sys/fs/cgroup/memory/bench0/memory.swappiness
echo 400M > /sys/fs/cgroup/memory/bench0/memory.limit_in_bytes
PREDSWAP_OFFLOAD_ONLY=1 /opt/predswap/predswapd run --m4-mode off \
  --cgroup /sys/fs/cgroup/memory/bench0 --budget-mb 400 --headroom-mb 64 &
sh -c 'echo 0 > /sys/fs/cgroup/memory/bench0/tasks; LD_PRELOAD=/opt/predswap/predswap_agent.so exec "$@"' sh <负载命令及参数>
/opt/predswap/predswapd status
/opt/predswap/predswapd replay /tmp/t.psrec
```

完整 P：去掉 `PREDSWAP_OFFLOAD_ONLY=1` 和 `--m4-mode off`。默认保留内核 swap 预读，`PREDSWAP_REPLACE_RA=1` 才进入修改主机预读设置的消融模式。`bench/fixed_cb.sh B0 B1 B2 P` 以固定操作数、相同种子测量四组，不以节流后的吞吐或预取请求字节代替性能。

## 应用级评测（memcached）

```sh
gcc -O2 -Wall -pthread -o /opt/predswap/mcbench bench/mcbench.c -lm   # 目标机
sh bench/mc_slo.sh B0:4096 B1:2240 P:2240      # 每项 VARIANT:BUDGET_MB
```

`mc_slo.sh` 每轮重启 memcached（`-m 3072 -t 4`）并在 4 GiB 限额下用 `mcbench preload` 灌满约 2 GB 数据，再把 cgroup 限额压到目标值、静置 5 s，最后用固定 2,000,000 次 GET:SET=95:5 的 Zipf 访问测稳态。B0 不做压缩。SLO 门限以 B0 为准：吞吐 ≥95%，p99 ≤2×。结果 CSV 与每轮 daemon 状态、负载输出都在 `artifacts=` 目录；`err` 非零的轮次不得进入统计。

要点(全部有 Phase 0 实测依据):
- bridge 需要 `ulimit -l unlimited`;daemon 自己 setrlimit。
- H 余量是机制必需:顶限额时 WILLNEED 静默无效(Phase 0 §2-6)。
- `ev_dropped` 证据链:bridge 退出时 stderr 打印 `dbg total/vmf_ok/filter_pass/rb_fail`。
- kill -9 predswapd:应用不受影响;bridge 因 stdout EPIPE 自行退出;系统退化为原生 swap(G6)。
