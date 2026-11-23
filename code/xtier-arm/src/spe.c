/* SPDX-License-Identifier: GPL-2.0 */
/*
 * spe.c — Arm SPE sampler for xtier-arm.
 *
 * Replaces xTier's PEBS per-task events. On this 5.10 aarch64 kernel SPE
 * records go to the per-event AUX buffer and are decoded in userspace (the
 * kernel does not synthesize per-sample perf events for aux PMUs, so a BPF
 * perf_event attach cannot see sample addresses). The per-task attach
 * (pid=tid, cpu=-1) gives pid attribution for free, mirroring xTier's
 * open_perf_for_threads().
 *
 * Record decoding follows the upstream perf tool's arm-spe packet decoder
 * (tools/perf/util/arm-spe-pkt-decoder.c, GPL-2.0, (c) 2017-2018 Arm Ltd):
 *   header byte: bits[3:0] type, bits[5:4] payload length (1<<n bytes)
 *   0x0 pad, 0x1 end, 0x71 timestamp, 0x42/0x43 events/data-source,
 *   0x64 context, 0x48 op-type, 0xb0/0xb8 address, 0x98/0x9a counter.
 * A record is a packet sequence terminated by TIMESTAMP or END. We extract
 * the DATA_VIRT address of LOAD/ST operations.
 *
 * AUX mmap protocol (kernel/events/core.c perf_mmap): mmap the data ring at
 * pgoff 0, WRITE aux_offset/aux_size into the mmap_page, then mmap the aux
 * region at that offset. Verified empirically on 192.168.90.7.
 */
#include "xtierd.h"
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <unistd.h>

#ifndef __NR_perf_event_open
#include <sys/syscall.h>
#endif
#define perf_open(a, pid, cpu, gfd, fl) (int)syscall(__NR_perf_event_open, (a), (pid), (cpu), (gfd), (fl))

#define DATA_PAGES 16            // power of two
#define AUX_PAGES  64            // power of two
#define MAX_RINGS  512
#define SPE_PATH   "/sys/bus/event_source/devices/arm_spe_0"

struct spe_ring {
    int    tid;
    int    fd;
    struct perf_event_mmap_page *mp;
    void  *aux;
    uint64_t aux_size;
    // in-flight record
    uint64_t virt_addr;
    int      is_store;
    int      have_op;
    int      have_addr;
    uint64_t nrec;
};

static struct spe_ring rings[MAX_RINGS];
static int n_rings;
static int spe_type = -1;
static uint64_t total_samples;

int spe_init(void) {
    char buf[64];
    int fd = open(SPE_PATH "/type", O_RDONLY);
    if (fd < 0) { perror("open " SPE_PATH "/type"); return -1; }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { fprintf(stderr, "spe: read type failed\n"); return -1; }
    buf[n] = '\0';
    spe_type = atoi(buf);
    printf("[*] SPE PMU type=%d interval=%d stores=%d\n", spe_type, G.spe_interval, G.spe_stores);
    return 0;
}

static int spe_open_tid(int tid) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = (uint32_t)spe_type;
    // config: bit0 ts_enable, bit33 load_filter, bit34 store_filter
    attr.config = (1ULL << 0) | (1ULL << 33) | (G.spe_stores ? (1ULL << 34) : 0);
    attr.exclude_kernel = 1;       // user accesses only (matches xTier)
    attr.sample_period = (uint64_t)G.spe_interval;  // SPE sampling interval (ops)
    attr.disabled = 1;

    int fd = perf_open(&attr, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd <= 0) return -1;

    size_t page = 4096;
    void *base = mmap(NULL, (1 + DATA_PAGES) * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return -1; }
    struct perf_event_mmap_page *mp = base;
    mp->aux_offset = (1 + DATA_PAGES) * page;
    mp->aux_size = AUX_PAGES * page;
    void *aux = mmap(NULL, mp->aux_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mp->aux_offset);
    if (aux == MAP_FAILED) { munmap(base, (1 + DATA_PAGES) * page); close(fd); return -1; }

    struct spe_ring *r = &rings[n_rings++];
    r->tid = tid; r->fd = fd; r->mp = mp; r->aux = aux; r->aux_size = mp->aux_size;
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    return 0;
}

// one event per thread (xTier open_perf_for_threads); rescan each epoch so
// OpenMP pools created later are covered too.
void spe_rescan_threads(void) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", (int)G.pid);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int tid = atoi(ent->d_name);
        if (tid <= 0 || n_rings >= MAX_RINGS) continue;
        int seen = 0;
        for (int i = 0; i < n_rings; i++) if (rings[i].tid == tid) { seen = 1; break; }
        if (!seen) spe_open_tid(tid);
    }
    closedir(d);
}

void spe_close_dead(void) {
    for (int i = 0; i < n_rings; ) {
        char p[64];
        snprintf(p, sizeof(p), "/proc/%d/task/%d", (int)G.pid, rings[i].tid);
        if (access(p, F_OK) != 0) {
            munmap(rings[i].aux, rings[i].aux_size);
            munmap(rings[i].mp, (1 + DATA_PAGES) * 4096);
            close(rings[i].fd);
            rings[i] = rings[--n_rings];
            continue;
        }
        i++;
    }
}

static inline unsigned payload_len(unsigned char h) { return 1u << ((h & 0x30) >> 4); }
static inline uint64_t ld_le(const unsigned char *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// Decode one record group; emit a sample per LOAD/ST record carrying a
// DATA_VIRT address. Returns bytes consumed or negative on malformed input.
static int spe_decode_buf(struct spe_ring *r, const unsigned char *buf, size_t len,
                          spe_sample_fn cb, void *ctx) {
    size_t pos = 0;
    while (pos < len) {
        const unsigned char *p = buf + pos;
        unsigned char h = p[0];
        size_t adv = 1;

        if (h == 0x0) { pos += 1; continue; }                    // pad
        if (h == 0x1) { pos += 1; goto record_done; }            // end
        if ((h & 0xCF) == 0x42 || (h & 0xCF) == 0x43) {          // events / data source
            adv = 1 + payload_len(h);
        } else if ((h & 0xFC) == 0x64) {                          // context
            adv = 1 + payload_len(h);
        } else if ((h & 0xFC) == 0x48) {                          // op type
            unsigned n = payload_len(h);
            adv = 1 + n;
            if (pos + adv > len) break;
            uint64_t v = n ? ld_le(p + 1, n) : 0;
            // class LD_ST_ATOMIC; payload bit0 = store
            r->have_op = 1;
            r->is_store = (v & 1) ? 1 : 0;
        } else if ((h & 0xF8) == 0xb0 || (h & 0xF8) == 0x98) {   // address / counter (short hdr)
            unsigned idx = h & 7;
            unsigned n = payload_len(h);
            adv = 1 + n;
            if (pos + adv > len) break;
            if ((h & 0xF8) == 0xb0 && idx == 2) {                 // DATA_VIRT
                uint64_t v = ld_le(p + 1, n) & 0x00FFFFFFFFFFFFFFULL;
                // kernel-space fixup: byte6 high nibble 0xf -> top byte 0xff
                uint64_t b6 = (v >> 48) & 0xF0ULL;
                if (b6 == 0xF0ULL) v |= 0xFFULL << 56;
                r->virt_addr = v;
                r->have_addr = 1;
            }
        } else if ((h & 0xFC) == 0x20) {
            // extended header (0x20-0x23): second header byte carries type
            if (pos + 1 >= len) break;
            unsigned char h1 = p[1];
            unsigned idx = ((h & 3) << 3) | (h1 & 7);
            unsigned n = payload_len(h1);
            adv = 2 + n;
            if (pos + adv > len) break;
            if ((h1 & 0xF8) == 0xb0 && idx == 2) {
                uint64_t v = ld_le(p + 2, n) & 0x00FFFFFFFFFFFFFFULL;
                uint64_t b6 = (v >> 48) & 0xF0ULL;
                if (b6 == 0xF0ULL) v |= 0xFFULL << 56;
                r->virt_addr = v;
                r->have_addr = 1;
            }
        } else if (h == 0x71) {                                   // timestamp: record terminator
            adv = 1 + payload_len(h);
            if (pos + adv > len) break;
            goto record_done;
        } else {
            // unknown header: resync by skipping a byte
            pos += 1;
            continue;
        }

        if (pos + adv > len) break;   // partial packet at buffer edge
        pos += adv;
        continue;

record_done:
        if (r->have_op && r->have_addr && r->virt_addr) {
            cb(ctx, r->virt_addr & ~0xFFFULL, r->is_store);
            total_samples++;
        }
        r->have_op = r->have_addr = 0;
        r->virt_addr = 0;
        r->nrec++;
        if (pos + adv > len) return (int)pos;
        pos += adv;
    }
    return (int)pos;
}

void spe_drain_all(spe_sample_fn cb, void *ctx) {
    for (int i = 0; i < n_rings; i++) {
        struct spe_ring *r = &rings[i];
        struct perf_event_mmap_page *mp = r->mp;
        uint64_t head = __atomic_load_n(&mp->aux_head, __ATOMIC_ACQUIRE);
        uint64_t tail = __atomic_load_n(&mp->aux_tail, __ATOMIC_ACQUIRE);
        if (head == tail) continue;
        unsigned char *base = (unsigned char *)r->aux;
        uint64_t size = r->aux_size, mask = size - 1;
        // consume from tail to head; handle wrap by decoding in two slices
        if (head - tail <= size) {
            uint64_t t = tail & mask, n = head - tail;
            if (t + n <= size) {
                int used = spe_decode_buf(r, base + t, (size_t)n, cb, ctx);
                if (used < 0 || (uint64_t)used > n) used = (int)n;
                tail += (uint64_t)used;
            } else {
                uint64_t n1 = size - t, n2 = n - n1;
                int used = spe_decode_buf(r, base + t, (size_t)n1, cb, ctx);
                if (used <= 0) used = (int)n1;
                tail += (uint64_t)used;
                used = spe_decode_buf(r, base, (size_t)n2, cb, ctx);
                if (used <= 0) used = (int)n2;
                tail += (uint64_t)used;
            }
        } else {
            // head wrapped past tail (overrun): drop everything, resync at head
            tail = head;
        }
        __atomic_store_n(&mp->aux_tail, tail, __ATOMIC_RELEASE);
        // ack the data ring so poll() keeps waking
        __u64 dh = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
        __atomic_store_n(&mp->data_tail, dh, __ATOMIC_RELEASE);
    }
}

uint64_t spe_sample_count(void) { return total_samples; }
