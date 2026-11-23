// SPDX-License-Identifier: GPL-2.0
// xtier-arm fault channel: kprobe on do_swap_page, the one path every
// access to a swapped page must take. This is the observation channel the
// sampling-based design cannot provide (exp16c): kernel-swapped cold pages
// have no state in our table, so their fault-backs were invisible and the
// prefetch trigger never fired. The probe reports the faulting VA; the
// daemon turns it into the fault->prefetch signal.
//
// Ported from predswap's bpf/predswap.bpf.c (verified on this exact kernel,
// 5.10.0-216 aarch64; VMF_ADDR_OFF=24 confirmed by predswap's offprobe):
//  - no CO-RE: fixed kernel, hardcoded offset
//  - no kretprobe on this kernel (displaces the kprobe)
//  - our own process_vm_readv prefetch faults run in daemon context
//    (current = daemon tgid) and are filtered out by the watched map

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SEC(NAME) __attribute__((section(NAME), used))
#define __uint(name, val) int (*name)[val]
#define __type(name, val) typeof(val) *name

#define BPF_MAP_TYPE_ARRAY 2
#define BPF_MAP_TYPE_HASH 1
#define BPF_MAP_TYPE_RINGBUF 27

static void *(*bpf_map_lookup_elem)(const void *map, const void *key) = (void *)1;
static u64 (*bpf_get_current_pid_tgid)(void) = (void *)14;
static u64 (*bpf_ktime_get_ns)(void) = (void *)5;
static long (*bpf_probe_read)(void *dst, u32 size, const void *unsafe_ptr) = (void *)4;
static void *(*bpf_ringbuf_reserve)(void *ringbuf, u64 size, u64 flags) = (void *)131;
static void (*bpf_ringbuf_submit)(void *data, u64 flags) = (void *)132;

struct fault_ev {
    u64 ts_ns;
    u64 va;    /* 4K-aligned */
    u32 tgid;
    u32 tid;
}; /* natural alignment, sizeof = 24 */

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1024 * 1024);
} fault_rb SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8);
    __type(key, u32);
    __type(value, u32);
} watched SEC(".maps");

/* arm64 kprobe ctx = struct pt_regs{u64 regs[31]; ...}; do_swap_page(vmf): x0 */
#define VMF_ADDR_OFF 24 /* offsetof(struct vm_fault, address), offprobe-verified */

SEC("kprobe/do_swap_page")
int on_swapin(void *ctx)
{
    u64 vmf = ((u64 *)ctx)[0];
    u64 va;
    if (bpf_probe_read(&va, sizeof(va), (void *)(vmf + VMF_ADDR_OFF)))
        return 0;
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tgid = (u32)(pid_tgid >> 32);
    if (!bpf_map_lookup_elem(&watched, &tgid))
        return 0;
    struct fault_ev *e = bpf_ringbuf_reserve(&fault_rb, sizeof(*e), 0);
    if (!e)
        return 0;
    e->ts_ns = bpf_ktime_get_ns();
    e->va = va & ~4095ULL;
    e->tgid = tgid;
    e->tid = (u32)pid_tgid;
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
