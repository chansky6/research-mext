/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec.c — migration executor for xtier-arm (DRAM -> SSD-swap).
 * Replaces xTier's kernel module + xtier_migrate_range + PTE A-bit scanner:
 *   demotion  : process_madvise(pidfd, MADV_PAGEOUT) — 5.10 whitelist, verified
 *   promotion : kernel demand paging (major fault) — by design (see README)
 *   cold scan : /proc/pid/pagemap + /sys/kernel/mm/page_idle/bitmap
 *   tier truth: pagemap present/swap bits
 */
#include "xtierd.h"
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#define MAX_IOV 1024              // UIO_MAXIOV: process_madvise iovcnt cap

struct exec_stats XSTATS;

static uint32_t storm_threshold = 16000;   // XTIER_STORM_PSWPIN; ON above the ~12K demand-churn floor
static uint64_t storm_ewma_pswpin;        // EWMA of swap-in pages/s

static void read_vmstat_cum(uint64_t *pswpin, uint64_t *pswpout) {
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) { *pswpin = *pswpout = 0; return; }
    char k[64]; unsigned long long v;
    while (fscanf(f, "%63s %llu", k, &v) == 2) {
        if (!strcmp(k, "pswpin")) *pswpin = v;
        else if (!strcmp(k, "pswpout")) *pswpout = v;
    }
    fclose(f);
}


static int pidfd = -1;
static int pagemap_fd = -1;
static int pageidle_fd = -1;

void exec_init(void) {
    if (G.storm_pswpin > 0) storm_threshold = G.storm_pswpin;
    char p[64];
    pidfd = (int)syscall(SYS_pidfd_open, (int)G.pid, 0);
    if (pidfd < 0) { perror("pidfd_open"); exit(1); }
    snprintf(p, sizeof(p), "/proc/%d/pagemap", (int)G.pid);
    pagemap_fd = open(p, O_RDONLY);
    pageidle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);  // must be RDWR
    printf("[*] exec: pidfd=%d pagemap=%d page_idle=%d\n", pidfd, pagemap_fd, pageidle_fd);
}

// ---- RSS / budget ----
uint64_t exec_resident_pages(void) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/statm", (int)G.pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    unsigned long size, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    fclose(f);
    return resident;
}

// ---- demotion: sort, merge contiguous, MADV_PAGEOUT in <=1024-iovec calls ----
static int addr_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// returns pages actually handed to the kernel (attempted), or -1
static long demote_range_batch(uint64_t *addrs, uint32_t n) {
    qsort(addrs, n, sizeof(uint64_t), addr_cmp);
    struct iovec iovs[MAX_IOV];
    uint32_t n_iov = 0, i = 0;
    long attempted = 0;
    while (i < n && n_iov < MAX_IOV) {
        while (i + 1 < n && addrs[i + 1] == addrs[i]) { i++; n--; }   // dedup after sort
        uint64_t start = addrs[i], end = start + 4096, cnt = 1;
        while (i + cnt < n && addrs[i + cnt] == end) { end += 4096; cnt++; }
        iovs[n_iov++] = (struct iovec){ .iov_base = (void *)(uintptr_t)start, .iov_len = end - start };
        attempted += (long)cnt;
        i += cnt;
    }
    long ret = syscall(SYS_process_madvise, pidfd, iovs, n_iov, MADV_PAGEOUT, 0);
    XSTATS.demote_att += attempted;
    if (ret < 0) { XSTATS.demote_fail += attempted; return -1; }
    XSTATS.demote_ok += attempted;
    return attempted;
}

// demote a list of candidate pages; sets state->tier=2 + cooldown on success
void exec_demote(uint64_t *addrs, uint32_t n) {
    // dedup + stack/exec guard
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t pg = addrs[i];
        const struct meta_val *mv = pol_meta_get(pg);
        if (mv && (mv->page_type == 3 || mv->page_type == 1)) continue;
        if (i > 0 && pg == addrs[i - 1]) continue;   // after sort in caller? no: dedup naively
        addrs[m++] = pg;
    }
    if (m == 0) return;
    long done = demote_range_batch(addrs, m);
    if (done < 0) return;
    // mark state: swapped out, cooldown anchor = this epoch
    uint32_t ep = pol_cur_epoch();
    for (long i = 0; i < done; i++) {
        struct pstate *st = pol_state_get(addrs[i], 0);
        if (st) { st->tier = 2; st->last_mig_epoch = ep; }
    }
}

// ---- page_idle cold scan (predswap-validated flow on this machine) ----
// Walk the target's anon VMAs via pagemap; for each present page read the
// page_idle bit: 1 = idle since we last marked it -> cold candidate. Bits of
// accessed pages read 0 and are re-marked idle for the next round.
// Chunked: one call scans at most G.cold_scan_chunk PTEs, resuming next call
// (mirrors xTier's COLD_SCAN_MAX_PTES chunking).
static uint64_t scan_resume_addr;
static int scan_first_pass = 1;

static int idle_bit_get_set(uint64_t pfn, int write_one, int *out) {
    // bitmap layout: word = pfn/64, bit = pfn%64 (8 bytes per 64 pages)
    uint64_t word;
    off_t off = (off_t)(pfn / 64) * 8;
    if (pread(pageidle_fd, &word, 8, off) != 8) return -1;
    if (write_one && !((word >> (pfn % 64)) & 1)) {
        word |= 1ULL << (pfn % 64);
        if (pwrite(pageidle_fd, &word, 8, off) != 8) return -1;
    }
    *out = (int)((word >> (pfn % 64)) & 1);
    return 0;
}

static uint32_t pagemap_present(uint64_t page, uint64_t *pfn_out) {
    uint64_t ent;
    off_t off = (off_t)(page / 4096) * 8;
    if (pread(pagemap_fd, &ent, 8, off) != 8) return 0;
    *pfn_out = ent & 0x007FFFFFFFFFFFFFULL;
    return (ent >> 63) & 1;
}

// scan one chunk; appends cold candidates to out[]. returns count scanned.
static uint32_t cold_scan_chunk(uint64_t *out, uint32_t max_out, uint32_t *n_out) {
    if (pageidle_fd < 0 || pagemap_fd < 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)G.pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char *line = NULL; size_t cap = 0;
    uint32_t scanned = 0;
    uint64_t addr = scan_resume_addr;
    if (addr == 0) { scan_resume_addr = 0; }

    while (getline(&line, &cap, fp) > 0) {
        unsigned long s, e; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %4s", &s, &e, perms) != 3) continue;
        char *pathname = strchr(line, '/');
        if (pathname && strncmp(pathname, "/dev/", 5) == 0) continue;
        if (addr >= e) continue;
        if (addr < s) addr = s;
        if (strchr(perms, 'w') == NULL) { if (addr >= s && addr < e) addr = e; continue; }  // writable anon-ish only
        while (addr < e && scanned < G.cold_scan_chunk && *n_out < max_out) {
            uint64_t pfn;
            if (pagemap_present(addr, &pfn)) {
                int bit = 0;
                if (idle_bit_get_set(pfn, scan_first_pass ? 1 : 0, &bit) == 0) {
                    if (!scan_first_pass && bit) out[(*n_out)++] = addr;   // stayed idle -> cold
                    else if (scan_first_pass) { /* marked idle, wait for next pass */ }
                }
                scanned++;
            }
            addr += 4096;
        }
        if (scanned >= G.cold_scan_chunk || *n_out >= max_out) break;
    }
    free(line); fclose(fp);
    XSTATS.cold_scanned += scanned;
    XSTATS.cold_found += *n_out;
    if (addr == 0 || addr >= (1UL << 47)) {
        scan_resume_addr = 0;
        scan_first_pass = 0;
        XSTATS.cold_passes++;
    } else {
        scan_resume_addr = addr;
    }
    return scanned;
}

// ---- 1Hz: majflt / swap traffic ----
static uint64_t last_majflt, last_pswpin, last_pswpout;
static void read_vmstat(uint64_t *pswpin, uint64_t *pswpout) { read_vmstat_cum(pswpin, pswpout); }
static uint64_t read_majflt(void) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", (int)G.pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    // skip 11 fields then majflt (field 12)
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    char *c = buf; int field = 0;
    while (field < 11 && c) { c = strchr(c + 1, ' '); field++; }
    return c ? strtoull(c + 1, NULL, 10) : 0;
}

void exec_second(FILE *stats_csv) {
    XSTATS.majflt_cum = read_majflt();
    uint64_t pi = 0, po = 0;
    read_vmstat(&pi, &po);
    XSTATS.pswpin_cum = pi;
    XSTATS.pswpout_cum = po;
    uint64_t dmaj = XSTATS.majflt_cum - last_majflt;
    uint64_t dso = po - last_pswpout;
    uint64_t dsi = pi - last_pswpin;
    last_majflt = XSTATS.majflt_cum; last_pswpin = pi; last_pswpout = po;
    if (stats_csv) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stats_csv, "%ld.3,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                ts.tv_sec, (unsigned long long)dmaj, (unsigned long long)dsi, (unsigned long long)dso,
                (unsigned long long)exec_resident_pages(), (unsigned long long)XSTATS.demote_ok,
                (unsigned long long)XSTATS.demote_fail, (unsigned long long)XSTATS.cold_found,
                (unsigned long long)XSTATS.majflt_cum, (unsigned long long)XSTATS.cold_scanned);
        fflush(stats_csv);
    }
}

// ---- per-epoch: budget enforcement ----
void exec_epoch(uint32_t ep_fin) {
    if (G.mode != 3) return;
    if (G.budget_pages == 0) return;

    // Fix 4 (fault-storm stand-down): when swap-in rate dwarfs what the SSD
    // can sustain, every daemon demotion just re-poisons the swapped set with
    // hot pages (exp6b: pswpin collapsed-sampling spiral reached 26K faults/s,
    // 2.2x worse than kernel LRU). Stand down the ML channel; the idle-bit
    // cold scan keeps running (hardware evidence), the kernel owns the rest.
    uint64_t pi_cum = 0, po_cum = 0;
    read_vmstat_cum(&pi_cum, &po_cum);
    int storm_on;
    {
        static uint64_t last_pi;
        static struct timespec last_t;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - last_t.tv_sec) + (now.tv_nsec - last_t.tv_nsec) / 1e9;
        if (dt < 0.05) dt = 0.05;
        double cur = (double)(pi_cum - last_pi) / dt;   // pages/s
        last_pi = pi_cum; last_t = now;
        storm_ewma_pswpin = (uint64_t)(storm_ewma_pswpin * 0.7 + cur * 0.3);
        // Hysteresis state machine. The demand-churn floor at this budget
        // measured ~10-12K pswpin/s on every config (exp5/6/7), so the ON
        // threshold sits above it: the gate must catch the 26K spiral
        // (exp6b), not the irreducible churn. OFF threshold keeps it from
        // oscillating at the floor.
        static int storm_state = 0;
        if (!storm_state && storm_ewma_pswpin > storm_threshold) {
            storm_state = 1;
            printf("[STORM] ON  (pswpin ewma=%llu > %u) -- ML channel standing down\n",
                   (unsigned long long)storm_ewma_pswpin, storm_threshold);
        } else if (storm_state && storm_ewma_pswpin < storm_threshold * 85 / 100) {
            storm_state = 0;
            printf("[STORM] OFF (pswpin ewma=%llu) -- ML channel rearming\n",
                   (unsigned long long)storm_ewma_pswpin);
        }
        if (storm_state) XSTATS.storm_epochs++;
        storm_on = storm_state;
    }

    uint64_t resident = exec_resident_pages();
    if (resident <= G.budget_pages) return;
    uint64_t over = resident - G.budget_pages;

    // Cold-scan demotion ALWAYS runs (hardware idle-bit evidence, not
    // sample-based): exp7's early-return gated it too, freezing the daemon
    // completely.
    static uint64_t cold_out[1024];
    uint32_t n_cold = 0;
    cold_scan_chunk(cold_out, 1024, &n_cold);

    uint64_t budget = G.mig_budget;
    if (n_cold > 0 && budget > 0) {
        uint32_t n = n_cold < budget ? n_cold : (uint32_t)budget;
        exec_demote(cold_out, n);
        budget -= n;
    }
    if (storm_on) return;   // fault storm: only hardware-evidence demotion continues
    if (budget > 0) {
        // take the lowest-scored ML candidates, in 1024-page chunks
        // (MAX_IOV iovecs per process_madvise call), until the epoch budget
        // or the candidate pool is exhausted.
        struct dem_cand *pool = pol_dem_pool();
        uint32_t pn = pol_dem_n();
        if (pool && pn > 0) {
            static uint64_t addrs[1024];
            uint32_t i = 0;
            while (i < pn && budget > 0) {
                uint32_t n = 0;
                while (i < pn && n < 1024 && budget > 0) {
                    addrs[n++] = pool[i++].page;
                    budget--;
                }
                exec_demote(addrs, n);
            }
        }
    }
    (void)ep_fin; (void)over;
}
