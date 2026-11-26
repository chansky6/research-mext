/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef SSDPAGER_H
#define SSDPAGER_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* [addr, addr+len) of process pid; done = pages actually moved (out) */
struct sp_range {
	__s32 pid;
	__u32 pad;
	__u64 addr;
	__u64 len;
	__u64 done;
};

#define SP_EVICT	_IOWR('S', 1, struct sp_range)	/* DRAM -> SSD */
#define SP_PROMOTE	_IOWR('S', 2, struct sp_range)	/* SSD -> DRAM, no fault */
#define SP_EVACUATE	_IO('S', 3)			/* bring every page back */

/*
 * Scattered pages of process pid: addrs points to nr page addresses
 * (nr <= 65536), handled in array order.  deadline_ns is CLOCK_MONOTONIC;
 * once it has passed, entries not yet started are dropped (0 = no deadline).
 */
struct sp_vec {
	__s32 pid;
	__u32 nr;
	__u64 addrs;
	__u64 deadline_ns;
	__u64 done;		/* out: pages actually moved */
	__u64 dropped;		/* out: entries skipped for the deadline */
};

#define SP_EVICTV	_IOWR('S', 4, struct sp_vec)
#define SP_PROMOTEV	_IOWR('S', 5, struct sp_vec)

/* Where each page is; states points to nr bytes (out). */
enum {
	SP_PG_NONE,	/* never touched, or not mapped */
	SP_PG_SSD,
	SP_PG_OLD,	/* in DRAM, not accessed since it was mapped */
	SP_PG_YOUNG,	/* in DRAM, accessed */
	SP_PG_BUSY,	/* being migrated right now */
	SP_PG_OTHER,	/* some other non-present entry, e.g. swap */
};

struct sp_query {
	__s32 pid;
	__u32 nr;
	__u64 addrs;
	__u64 states;
};

#define SP_QUERY	_IOW('S', 6, struct sp_query)

enum {
	SP_EV_FAULT = 1,	/* demand fault on an SSD page; lat_ns = service time */
	SP_EV_LATE,		/* access blocked behind an in-flight promotion */
	SP_EV_EVICT_WAIT,	/* access blocked behind an in-flight eviction */
};

struct sp_event {
	__u64 ts_ns;		/* CLOCK_MONOTONIC, start of the event */
	__u64 addr;		/* page address */
	__s32 pid;		/* thread group that touched the page */
	__u32 type;
	__u64 lat_ns;
};

/* Drain up to nr (<= 4096) events into buf, oldest first. */
struct sp_trace {
	__u64 buf;
	__u32 nr;
	__u32 got;		/* out */
	__u64 lost;		/* out: events overwritten since module load */
};

#define SP_TRACE	_IOWR('S', 7, struct sp_trace)

/*
 * State of every page in [addr, addr+len) of process pid, one byte per page
 * (len <= 1 GiB).  With SP_SCAN_CLEAR the access flag of the private anonymous
 * pages reported as SP_PG_YOUNG is cleared, so the next scan tells which pages
 * were touched in between.
 */
#define SP_SCAN_CLEAR	1

struct sp_scan {
	__s32 pid;
	__u32 flags;
	__u64 addr;
	__u64 len;
	__u64 states;
};

#define SP_SCAN		_IOW('S', 8, struct sp_scan)

#endif
