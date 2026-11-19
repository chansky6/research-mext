// SPDX-License-Identifier: GPL-2.0
// predswap eBPF:双 entry kprobe 事件流(依据《predswap-phase0-探测报告.md》)。
//
//  - 不用 kretprobe:本内核(5.10 arm64)上 r: 事件会挤掉同函数的 p: 事件(§2-9);
//    分类靠 miss 配对,由用户态分类器完成(规格 v0.2 §4.1)。
//  - do_swap_page 每次访问换出页必经;swapin_readahead 仅 cache-miss 路径调用。
//    而 WILLNEED 走 read_swap_cache_async 逐页,不经过它(5.15 madvise.c:187-217,
//    机制同 5.10)——预取自身不产生 miss 事件,归因干净。
//  - 不使用 CO-RE:目标内核固定(见 bpf/offprobe.c,vm_fault.address 偏移 = 24)。
//    换内核需重跑 offprobe 并重编译本文件。
//  - 事件结构自然对齐,sizeof = 32(尾部填充);bridge 与 daemon 必须用同一布局。

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SEC(NAME) __attribute__((section(NAME), used))
#define __uint(name, val) int (*name)[val]
#define __type(name, val) typeof(val) *name
#define __always_inline inline __attribute__((always_inline))

#define BPF_MAP_TYPE_HASH 1
#define BPF_MAP_TYPE_ARRAY 2
#define BPF_MAP_TYPE_RINGBUF 27

/* helper 函数表(编号 = uapi linux/bpf.h) */
static void *(*bpf_map_lookup_elem)(const void *map, const void *key) = (void *)1;
static u64 (*bpf_get_current_pid_tgid)(void) = (void *)14;
static u64 (*bpf_ktime_get_ns)(void) = (void *)5;
static long (*bpf_probe_read)(void *dst, u32 size, const void *unsafe_ptr) = (void *)4;
static void *(*bpf_ringbuf_reserve)(void *ringbuf, u64 size, u64 flags) = (void *)131;
static void (*bpf_ringbuf_submit)(void *data, u64 flags) = (void *)132;

#define EV_SWAPIN 0 /* do_swap_page 入口 */
#define EV_MISS 1   /* swapin_readahead 入口 = cache miss */

struct swap_ev {
    u64 ts_ns; /* ktime_get_ns */
    u64 va;    /* PAGE_MASK 后 */
    u32 tgid;
    u32 tid;
    u8 kind;
    u8 _pad[3];
}; /* 自然对齐,sizeof = 32(注意尾部填充,消费端按 32 解) */

struct dbg_val {
    u64 total;       /* kprobe 命中总数(过滤前) */
    u64 vmf_ok;      /* vmf->address 读取成功 */
    u64 filter_pass; /* watched 表命中 */
    u64 rb_fail;     /* ringbuf reserve 失败(= ev_dropped) */
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 8 * 1024 * 1024);
} events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64); /* P1:单实验托管进程数 */
    __type(key, u32);
    __type(value, u32);
} watched SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct dbg_val);
} dbg SEC(".maps");

/* arm64 kprobe ctx = struct pt_regs{u64 regs[31]; ...}; x0=arg1, x2=arg3 */
#define VMF_ADDR_OFF 24 /* offsetof(struct vm_fault, address), offprobe 实测 */

static __always_inline void report(void *ctx, int arg, u8 kind)
{
    u32 zero = 0;
    struct dbg_val *d = bpf_map_lookup_elem(&dbg, &zero);

    if (d)
        d->total++;

    u64 vmf = ((u64 *)ctx)[arg];
    u64 va;
    if (bpf_probe_read(&va, sizeof(va), (void *)(vmf + VMF_ADDR_OFF)))
        return;
    if (d)
        d->vmf_ok++;

    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tgid = pid_tgid >> 32;
    if (!bpf_map_lookup_elem(&watched, &tgid))
        return;
    if (d)
        d->filter_pass++;

    struct swap_ev *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        if (d)
            d->rb_fail++;
        return;
    }
    e->ts_ns = bpf_ktime_get_ns();
    e->va = va & ~4095ULL;
    e->tgid = tgid;
    e->tid = (u32)pid_tgid;
    e->kind = kind;
    bpf_ringbuf_submit(e, 0);
}

/* do_swap_page(struct vm_fault *vmf): vmf = x0 */
SEC("kprobe/do_swap_page")
int on_swapin(void *ctx)
{
    report(ctx, 0, EV_SWAPIN);
    return 0;
}

/* swapin_readahead(swp_entry_t entry, gfp_t gfp, struct vm_fault *vmf): vmf = x2 */
SEC("kprobe/swapin_readahead")
int on_miss(void *ctx)
{
    report(ctx, 2, EV_MISS);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
