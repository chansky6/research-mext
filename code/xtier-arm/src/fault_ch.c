/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fault_ch.c — fault channel: loads the do_swap_page kprobe BPF object and
 * turns swap-in faults of the watched process into prefetch triggers.
 * Ported loader flow from predswap's bridge (same machine, libbpf 0.8.1).
 */
#include "xtierd.h"
#include <errno.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

struct fault_ev {
    uint64_t ts_ns;
    uint64_t va;
    uint32_t tgid;
    uint32_t tid;
};

static struct ring_buffer *fault_rb;
static struct bpf_object *fault_obj;

void pref_note_fault(uint64_t page);

static int fault_cb(void *ctx, void *data, size_t sz)
{
    (void)ctx;
    if (sz < sizeof(struct fault_ev)) return 0;
    const struct fault_ev *e = data;
    struct pstate *st = pol_state_get(e->va, 0);
    if (st) {
        // fault-back observed for a tracked page: resident again + pin, so
        // the demotion path leaves it alone for the cooldown window
        st->tier = 1;
        st->last_mig_epoch = pol_cur_epoch();
    }
    // region-level prefetch trigger (suppression rules live in the worker)
    pref_note_fault(e->va);
    return 0;
}

int fault_ch_init(pid_t pid)
{
    // 5.10 has no memcg-based BPF memory accounting: BPF maps are charged to
    // RLIMIT_MEMLOCK (predswap documented the same requirement).
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);
    const char *p = getenv("XTIER_BPF_OBJ");
    char path[512];
    if (p && *p) {
        snprintf(path, sizeof(path), "%s", p);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 32);
        if (n <= 0) { snprintf(path, sizeof(path), "fault_kern.o"); return -1; }
        path[n] = '\0';
        char *slash = strrchr(path, '/');
        strcpy(slash ? slash + 1 : path, "fault_kern.o");
    }
    fault_obj = bpf_object__open_file(path, NULL);
    if (libbpf_get_error(fault_obj)) {
        fprintf(stderr, "[!] fault_ch: open %s failed (prefetch will be blind)\n", path);
        fault_obj = NULL;
        return -1;
    }
    if (bpf_object__load(fault_obj)) {
        fprintf(stderr, "[!] fault_ch: BPF load failed (%s)\n", strerror(errno));
        bpf_object__close(fault_obj); fault_obj = NULL;
        return -1;
    }
    struct bpf_program *prog = bpf_object__find_program_by_name(fault_obj, "on_swapin");
    if (!prog || !bpf_program__attach(prog)) {
        fprintf(stderr, "[!] fault_ch: kprobe attach failed\n");
        return -1;
    }
    int fd_w = bpf_object__find_map_fd_by_name(fault_obj, "watched");
    int fd_rb = bpf_object__find_map_fd_by_name(fault_obj, "fault_rb");
    if (fd_w < 0 || fd_rb < 0) return -1;
    uint32_t key = (uint32_t)pid, val = 1;
    if (bpf_map_update_elem(fd_w, &key, &val, BPF_ANY)) return -1;
    fault_rb = ring_buffer__new(fd_rb, fault_cb, NULL, NULL);
    if (!fault_rb) return -1;
    printf("[*] fault_ch: do_swap_page kprobe armed for tgid=%d\n", (int)pid);
    return 0;
}

void fault_ch_poll(void)
{
    if (fault_rb) ring_buffer__consume(fault_rb);
}
