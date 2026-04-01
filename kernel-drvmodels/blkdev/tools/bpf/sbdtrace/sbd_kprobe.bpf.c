/*
 * Copyright (c) 2024 TECH VEDA(www.techveda.org)
 * Author: Raghu Bharadwaj
 *
 * This software is licensed under GPL v2.
 * See the accompanying LICENSE file for the full text.
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

/* Event types for function entry/exit tracing */
enum evtype {
	EV_ENTER = 10,
	EV_EXIT = 11
};

/* Structure to track function entry timestamp and request details */
struct start_t {
	__u64 ts_ns;              /* Entry timestamp */
	const struct request *rq; /* Request pointer */
	__u64 sector;             /* Starting sector */
	__u32 bytes;              /* Number of bytes */
};

/* Event structure sent from kernel to userspace */
struct event_t {
	__u64 ts_ns;   /* Event timestamp in nanoseconds */
	__u32 type;    /* Event type (enter or exit) */
	__u32 pid;     /* Process ID */
	__u64 rq_ptr;  /* Request pointer */
	__u64 sector;  /* Starting sector */
	__u32 bytes;   /* Number of bytes */
	__u64 dur_ns;  /* Duration in nanoseconds (only on EXIT) */
};

/* Ring buffer for sending events to userspace */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024); /* 256 KB buffer */
} events SEC(".maps");

/* Hash map to track function entry times per PID */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u32);
	__type(value, struct start_t);
} starts SEC(".maps");

/*
 * Trace function entry
 * Generic kprobe handler; target function is chosen during userspace attach
 */
SEC("kprobe")
int BPF_KPROBE(sbd_do_request_enter)
{
	struct start_t st = {};
	struct event_t *e;
	struct request *rq = (struct request *)PT_REGS_PARM1(ctx);
	__u32 pid = bpf_get_current_pid_tgid() >> 32;

	/* Record entry timestamp and request details */
	st.ts_ns = bpf_ktime_get_ns();
	st.rq = rq;
	st.sector = 0;
	st.bytes = 0;

	/*
	 * Best-effort read of request fields (kernel version dependent)
	 * In Linux 6.x, struct request has __sector and __data_len fields
	 */
	BPF_CORE_READ_INTO(&st.sector, rq, __sector);
	BPF_CORE_READ_INTO(&st.bytes, rq, __data_len);

	/* Store entry data for duration calculation on exit */
	bpf_map_update_elem(&starts, &pid, &st, BPF_ANY);

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	/* Populate entry event */
	e->ts_ns = st.ts_ns;
	e->type = EV_ENTER;
	e->pid = pid;
	e->rq_ptr = (unsigned long)rq;
	e->sector = st.sector;
	e->bytes = st.bytes;
	e->dur_ns = 0;

	/* Submit event to userspace */
	bpf_ringbuf_submit(e, 0);

	return 0;
}

/*
 * Trace function exit and calculate duration
 */
SEC("kretprobe")
int BPF_KRETPROBE(sbd_do_request_exit)
{
	__u32 pid = bpf_get_current_pid_tgid() >> 32;
	struct start_t *st;
	struct event_t *e;
	__u64 now = bpf_ktime_get_ns();

	/* Lookup entry timestamp for this PID */
	st = bpf_map_lookup_elem(&starts, &pid);
	if (!st)
		return 0;

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (e) {
		/* Populate exit event with duration */
		e->ts_ns = now;
		e->type = EV_EXIT;
		e->pid = pid;
		e->rq_ptr = (unsigned long)st->rq;
		e->sector = st->sector;
		e->bytes = st->bytes;
		e->dur_ns = now - st->ts_ns;

		/* Submit event to userspace */
		bpf_ringbuf_submit(e, 0);
	}

	/* Clean up entry data */
	bpf_map_delete_elem(&starts, &pid);

	return 0;
}

