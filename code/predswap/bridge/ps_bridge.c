// SPDX-License-Identifier: GPL-2.0
// ps_bridge:predswap eBPF 加载器。在目标机上运行(libbpf,头文件来自 libbpf-devel)。
//   - 加载 predswap.bpf.o,attach 两个 kprobe;
//   - 二进制事件流(sizeof(struct swap_ev) = 32 字节)写到 stdout(daemon 管道读取);
//   - stdin 行协议维护 watched tgid 过滤表:"+<pid>\n" / "-<pid>\n" / "quit\n"。
//   - 退出前把 dbg 计数器 dump 到 stderr(运行诊断与 ev_dropped)。
// 注意:本进程必须 RLIMIT_MEMLOCK 足够大(5.10 无 memcg BPF 记账,实测 64M 不够)。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

struct swap_ev {
    unsigned long long ts_ns, va;
    unsigned int tgid, tid;
    unsigned char kind, pad[3];
}; /* 与 bpf/predswap.bpf.c 一致;sizeof = 32 */

struct dbg_val {
    unsigned long long total, vmf_ok, filter_pass, rb_fail;
};

static volatile sig_atomic_t g_stop = 0;
static int g_watched_fd = -1;
static int g_dbg_fd = -1;

static void on_sig(int sig) { (void)sig; g_stop = 1; }

static int on_sample(void *ctx, void *data, size_t size)
{
    (void)ctx;
    if (size != sizeof(struct swap_ev))
        return 0;
    ssize_t n = write(STDOUT_FILENO, data, size);
    if (n != (ssize_t)sizeof(struct swap_ev)) {
        /* daemon 死亡(EPIPE 等写失败):退出,由 daemon 侧负责重启 */
        g_stop = 1;
    }
    return 0;
}

static void *stdin_thread(void *arg)
{
    (void)arg;
    char line[64];
    while (!g_stop && fgets(line, sizeof line, stdin)) {
        long pid;
        if (!strncmp(line, "quit", 4))
            break;
        int add = (line[0] == '+');
        if (!add && line[0] != '-')
            continue;
        pid = strtol(line + 1, NULL, 10);
        if (pid <= 0)
            continue;
        uint32_t k = (uint32_t)pid, v = 1;
        if (g_watched_fd >= 0) {
            int rc = add ? bpf_map_update_elem(g_watched_fd, &k, &v, 0)
                         : bpf_map_delete_elem(g_watched_fd, &k);
            fprintf(stderr, "bridge: watch %c%ld rc=%d\n", add ? '+' : '-', pid, rc);
        }
    }
    g_stop = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    const char *obj_path = argc > 1 ? argv[1] : "predswap.bpf.o";
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
    /* 保留 libbpf 默认日志(stderr):load 失败时必须能看到具体原因 */

    struct bpf_object *obj = bpf_object__open_file(obj_path, NULL);
    if (!obj) {
        fprintf(stderr, "bridge: open %s failed\n", obj_path);
        return 1;
    }
    if (bpf_object__load(obj)) {
        fprintf(stderr, "bridge: load failed (kprobe 名/偏移与内核不匹配?)\n");
        return 1;
    }
    int ev_fd = bpf_object__find_map_fd_by_name(obj, "events");
    g_watched_fd = bpf_object__find_map_fd_by_name(obj, "watched");
    g_dbg_fd = bpf_object__find_map_fd_by_name(obj, "dbg");
    if (ev_fd < 0 || g_watched_fd < 0) {
        fprintf(stderr, "bridge: map not found\n");
        return 1;
    }
    struct bpf_program *p_in = bpf_object__find_program_by_name(obj, "on_swapin");
    struct bpf_program *p_miss = bpf_object__find_program_by_name(obj, "on_miss");
    if (!p_in || !p_miss) {
        fprintf(stderr, "bridge: prog not found\n");
        return 1;
    }
    struct bpf_link *l1 = bpf_program__attach_kprobe(p_in, false, "do_swap_page");
    struct bpf_link *l2 = bpf_program__attach_kprobe(p_miss, false, "swapin_readahead");
    if (!l1 || !l2) {
        fprintf(stderr, "bridge: attach kprobe failed: do_swap_page=%p swapin_readahead=%p\n",
                (void *)l1, (void *)l2);
        return 1;
    }
    fprintf(stderr, "bridge: attached do_swap_page + swapin_readahead\n");

    struct ring_buffer *rb = ring_buffer__new(ev_fd, on_sample, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "bridge: ringbuf new failed\n");
        return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, stdin_thread, NULL);

    while (!g_stop) {
        int rc = ring_buffer__poll(rb, 200);
        if (rc == -EINTR)
            continue;
        if (rc < 0) {
            fprintf(stderr, "bridge: poll error %d\n", rc);
            break;
        }
    }
    ring_buffer__free(rb);

    if (g_dbg_fd >= 0) {
        struct dbg_val v;
        uint32_t zero = 0;
        if (!bpf_map_lookup_elem(g_dbg_fd, &zero, &v))
            fprintf(stderr, "bridge: dbg total=%llu vmf_ok=%llu filter_pass=%llu rb_fail=%llu\n",
                    v.total, v.vmf_ok, v.filter_pass, v.rb_fail);
    }
    bpf_link__destroy(l1);
    bpf_link__destroy(l2);
    bpf_object__close(obj);
    return 0;
}
