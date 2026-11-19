// SPDX-License-Identifier: GPL-2.0
// predswap-agent: LD_PRELOAD 侧车进程。接收 predswapd 的预取请求,在目标进程内执行
// madvise(MADV_WILLNEED)。机制依据见 predswap-phase0-探测报告.md §2-2/§2-3:
// 5.10 的 process_madvise 不接受 WILLNEED,必须进程内执行。
//
// 协议(dgram,固定路径目录 /tmp/predswap):
//   HELLO  (u32 magic=1, u32 pid)                     agent -> daemon
//   REQ    (u32 magic=2, u32 nseg, seg[nseg]{u64 addr,u64 len})  daemon -> agent
//   ACK    (u32 magic=3, u32 pid, u64 req_bytes, u64 adv_bytes, s32 ret)  agent -> daemon
// 字节序:本机小端。结构体 8 字节对齐,pack 如下。

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <pthread.h>

#define PS_DIR "/tmp/predswap"
#define PS_AGENT_DIR PS_DIR "/agents"
#define PS_DAEMON_SOCK PS_DIR "/agent.sock"

#define MAGIC_HELLO 1u
#define MAGIC_REQ 2u
#define MAGIC_ACK 3u

typedef struct { uint32_t magic; uint32_t pid; } __attribute__((packed)) msg_hello;
typedef struct { uint32_t magic; uint32_t nseg; } __attribute__((packed)) msg_req_hdr;
typedef struct { uint64_t addr; uint64_t len; } seg64;
typedef struct {
    uint32_t magic;
    uint32_t pid;
    uint64_t req_bytes;
    uint64_t adv_bytes;
    int32_t ret;
    uint32_t pad;
} __attribute__((packed)) msg_ack;

static int g_sock = -1;
static char g_sockpath[108];
static volatile int g_stop = 0;

static void send_ack(uint64_t req_bytes, uint64_t adv_bytes, int ret)
{
    msg_ack ack = {MAGIC_ACK, (uint32_t)getpid(), req_bytes, adv_bytes, ret, 0};
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, PS_DAEMON_SOCK);
    /* 尽力而为;ACK 丢失由 daemon 侧"有效字节/请求字节"比值监控兜底 */
    sendto(g_sock, &ack, sizeof ack, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void *agent_main(void *arg)
{
    (void)arg;
    char buf[64 + 1024 * sizeof(seg64)];
    while (!g_stop) {
        ssize_t n = recv(g_sock, buf, sizeof buf, 0);
        if (n < 8) continue;
        msg_req_hdr *h = (msg_req_hdr *)buf;
        if (h->magic != MAGIC_REQ || h->nseg > 1024) continue;
        if ((uint64_t)n < 8 + (uint64_t)h->nseg * sizeof(seg64)) continue;
        seg64 *segs = (seg64 *)(buf + 8);
        uint64_t req = 0, adv = 0;
        int ret = 0;
        for (uint32_t i = 0; i < h->nseg; i++) {
            /* 对齐/范围防御:必须是页对齐正长度,防止畸形请求 */
            if (!segs[i].addr || !segs[i].len ||
                (segs[i].addr & 4095ULL) || (segs[i].len & 4095ULL)) { ret = -22; continue; }
            req += segs[i].len;
            long r = madvise((void *)(uintptr_t)segs[i].addr, segs[i].len, MADV_WILLNEED);
            if (r == 0) adv += segs[i].len;
            else ret = (int)errno ? (int)errno : -1;
        }
        send_ack(req, adv, ret);
    }
    return NULL;
}

__attribute__((constructor))
static void agent_ctor(void)
{
    /* fork 出的子进程不重复起 agent(daemon 托管列表决定);单进程单线程足够 */
    if (g_sock >= 0) return;
    g_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_sock < 0) return;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    snprintf(g_sockpath, sizeof g_sockpath, "%s/a-%d.sock", PS_AGENT_DIR, getpid());
    strcpy(sa.sun_path, g_sockpath);
    unlink(g_sockpath);
    if (bind(g_sock, (struct sockaddr *)&sa, sizeof sa) < 0) goto fail;
    msg_hello hello = {MAGIC_HELLO, (uint32_t)getpid()};
    struct sockaddr_un da = {0};
    da.sun_family = AF_UNIX;
    strcpy(da.sun_path, PS_DAEMON_SOCK);
    /* daemon 可能尚未就绪:失败不致命,daemon 侧靠扫描 agents 目录发现我们 */
    sendto(g_sock, &hello, sizeof hello, 0, (struct sockaddr *)&da, sizeof da);

    pthread_t t;
    if (pthread_create(&t, NULL, agent_main, NULL) == 0) {
        pthread_detach(t);
        return;
    }
fail:
    close(g_sock);
    g_sock = -1;
}

__attribute__((destructor))
static void agent_dtor(void)
{
    if (g_sock >= 0) {
        g_stop = 1;
        shutdown(g_sock, SHUT_RDWR);
        close(g_sock);
        unlink(g_sockpath);
    }
}
