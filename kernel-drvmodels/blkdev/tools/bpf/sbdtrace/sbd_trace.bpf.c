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

/*
 * Note: Tracepoint structures are defined in vmlinux.h
 * We'll use them directly from there
 */

/* Event types for block I/O tracing */
enum event_type {
	EV_ISSUE = 1,
	EV_DONE = 2,
	EV_MERGE_BACK = 3,
	EV_MERGE_FRONT = 4,
};

/* Configuration structure for device filtering */
struct dev_filter_t {
	__u32 major;
	__u32 minor;
};

/* Event structure sent from kernel to userspace */
struct event_t {
	__u64 ts_ns;      /* Timestamp in nanoseconds */
	__u32 type;       /* Event type (issue, done, merge) */
	__u32 pid;        /* Process ID */
	__u32 major;      /* Device major number */
	__u32 minor;      /* Device minor number */
	__u64 sector;     /* Starting sector */
	__u32 bytes;      /* Number of bytes */
	__s32 err;        /* Error code (for EV_DONE) */
	char rwbs[8];     /* Read/write flags from tracepoint */
	char comm[16];    /* Process name */
};

/* Ring buffer for sending events to userspace */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024); /* 256 KB buffer */
} events SEC(".maps");

/* Configuration map for device filtering */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct dev_filter_t);
} device_filter SEC(".maps");

/* Helper function to check if device matches filter */
static __always_inline int dev_matches(__u32 maj, __u32 min)
{
	__u32 key = 0;
	struct dev_filter_t *cfg = bpf_map_lookup_elem(&device_filter, &key);

	if (!cfg)
		return 0;

	/* If no filter is set, match all devices */
	if (!cfg->major && !cfg->minor)
		return 1;

	return cfg->major == maj && cfg->minor == min;
}

/* Helper function to split device number into major/minor */
static __always_inline void split_dev(__u32 dev, __u32 *maj, __u32 *min)
{
	/* Matches kernel's 20:12 maj:min split */
	*maj = dev >> 20;
	*min = dev & ((1u << 20) - 1);
}

/* Trace block request issue events */
SEC("tracepoint/block/block_rq_issue")
int tp__block_rq_issue(struct trace_event_raw_block_rq *ctx)
{
	struct event_t *e;
	__u32 maj, min;

	/* Extract and filter by device major/minor */
	split_dev(ctx->dev, &maj, &min);
	if (!dev_matches(maj, min))
		return 0;

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	/* Populate event data */
	e->ts_ns = bpf_ktime_get_ns();
	e->type = EV_ISSUE;
	e->pid = bpf_get_current_pid_tgid() >> 32;
	e->major = maj;
	e->minor = min;
	e->sector = BPF_CORE_READ(ctx, sector);
	e->bytes = BPF_CORE_READ(ctx, bytes);
	e->err = 0;

	/* Copy read/write operation flags */
	__builtin_memset(e->rwbs, 0, sizeof(e->rwbs));
	bpf_probe_read_kernel(e->rwbs, sizeof(e->rwbs), ctx->rwbs);

	/* Get process name */
	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	/* Submit event to userspace */
	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* Trace block request completion events */
SEC("tracepoint/block/block_rq_complete")
int tp__block_rq_complete(struct trace_event_raw_block_rq_completion *ctx)
{
	struct event_t *e;
	__u32 maj, min;

	/* Extract and filter by device major/minor */
	split_dev(ctx->dev, &maj, &min);
	if (!dev_matches(maj, min))
		return 0;

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	/* Populate event data */
	e->ts_ns = bpf_ktime_get_ns();
	e->type = EV_DONE;
	e->pid = bpf_get_current_pid_tgid() >> 32;
	e->major = maj;
	e->minor = min;
	e->sector = BPF_CORE_READ(ctx, sector);
	e->bytes = BPF_CORE_READ(ctx, nr_sector) * 512;  /* Convert sectors to bytes */
	e->err = BPF_CORE_READ(ctx, error);

	/* Copy read/write operation flags */
	__builtin_memset(e->rwbs, 0, sizeof(e->rwbs));
	bpf_probe_read_kernel(e->rwbs, sizeof(e->rwbs), ctx->rwbs);

	/* Get process name (not in completion struct, get from current) */
	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	/* Submit event to userspace */
	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* Trace block bio backmerge events (bio merged at end of request) */
SEC("tracepoint/block/block_bio_backmerge")
int tp__block_bio_backmerge(struct trace_event_raw_block_bio *ctx)
{
	struct event_t *e;
	__u32 maj, min;

	/* Extract and filter by device major/minor */
	split_dev(ctx->dev, &maj, &min);
	if (!dev_matches(maj, min))
		return 0;

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	/* Populate event data */
	e->ts_ns = bpf_ktime_get_ns();
	e->type = EV_MERGE_BACK;
	e->pid = bpf_get_current_pid_tgid() >> 32;
	e->major = maj;
	e->minor = min;
	e->sector = BPF_CORE_READ(ctx, sector);
	e->bytes = BPF_CORE_READ(ctx, nr_sector) * 512;  /* Convert sectors to bytes */
	e->err = 0;

	/* Copy operation flags and get process name */
	__builtin_memset(e->rwbs, 0, sizeof(e->rwbs));
	bpf_probe_read_kernel(e->rwbs, sizeof(e->rwbs), ctx->rwbs);
	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	/* Submit event to userspace */
	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* Trace block bio frontmerge events (bio merged at beginning of request) */
SEC("tracepoint/block/block_bio_frontmerge")
int tp__block_bio_frontmerge(struct trace_event_raw_block_bio *ctx)
{
	struct event_t *e;
	__u32 maj, min;

	/* Extract and filter by device major/minor */
	split_dev(ctx->dev, &maj, &min);
	if (!dev_matches(maj, min))
		return 0;

	/* Reserve space in ring buffer */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	/* Populate event data */
	e->ts_ns = bpf_ktime_get_ns();
	e->type = EV_MERGE_FRONT;
	e->pid = bpf_get_current_pid_tgid() >> 32;
	e->major = maj;
	e->minor = min;
	e->sector = BPF_CORE_READ(ctx, sector);
	e->bytes = BPF_CORE_READ(ctx, nr_sector) * 512;  /* Convert sectors to bytes */
	e->err = 0;

	/* Copy operation flags and get process name */
	__builtin_memset(e->rwbs, 0, sizeof(e->rwbs));
	bpf_probe_read_kernel(e->rwbs, sizeof(e->rwbs), ctx->rwbs);
	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	/* Submit event to userspace */
	bpf_ringbuf_submit(e, 0);

	return 0;
}

