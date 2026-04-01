/*
 * Copyright (c) 2024 TECH VEDA(www.techveda.org)
 * Author: Raghu Bharadwaj
 *
 * This software is licensed under GPL v2.
 * See the accompanying LICENSE file for the full text.
 *
 * Block Device Function Tracer - Userspace Program
 *
 * This program uses eBPF kprobes to trace function entry/exit events
 * for block device driver functions. It measures execution time and
 * captures request details.
 *
 * Default target: do_request() function in sbd_ramdisk driver
 */

#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* Event types for function tracing */
enum event_type {
	EV_ENTER = 10,		/* Function entry */
	EV_EXIT = 11,		/* Function exit */
};

/* Event structure received from kernel BPF program */
struct event_t {
	uint64_t ts_ns;		/* Event timestamp in nanoseconds */
	uint32_t type;		/* Event type (enter or exit) */
	uint32_t pid;		/* Process ID */
	uint64_t rq_ptr;	/* Request pointer (for correlation) */
	uint64_t sector;	/* Starting sector (LBA) */
	uint32_t bytes;		/* Number of bytes */
	uint64_t dur_ns;	/* Duration in nanoseconds (exit only) */
};

/* Signal handler flag for graceful exit */
static volatile sig_atomic_t exiting;

/*
 * Signal handler for SIGINT (Ctrl-C)
 * Sets the exiting flag to break the polling loop
 */
static void on_sigint(int sig)
{
	exiting = 1;
}

/*
 * Ring buffer event handler
 * Called for each event received from the BPF program
 *
 * @ctx: User context (unused)
 * @data: Pointer to event data
 * @sz: Size of event data
 * @return: 0 on success
 */
static int handle_event(void *ctx, void *data, size_t sz)
{
	const struct event_t *e = data;

	if (e->type == EV_ENTER) {
		/* Function entry event */
		printf("ENTER rq=%p lba=%llu bytes=%u pid=%u\n",
		       (void *)e->rq_ptr,
		       (unsigned long long)e->sector,
		       e->bytes,
		       e->pid);
	} else if (e->type == EV_EXIT) {
		/* Function exit event with duration */
		printf("EXIT  rq=%p lba=%llu bytes=%u dur_us=%llu pid=%u\n",
		       (void *)e->rq_ptr,
		       (unsigned long long)e->sector,
		       e->bytes,
		       (unsigned long long)(e->dur_ns / 1000),
		       e->pid);
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *func = argc > 1 ? argv[1] : "do_request";
	int err = 0;
	struct bpf_object *obj = NULL;
	struct bpf_program *kp = NULL, *krp = NULL, *p;
	struct bpf_link *l1 = NULL, *l2 = NULL;
	struct ring_buffer *rb = NULL;
	char obj_path[1024];
	char *prog_dir;

	/* Find BPF object in same directory as executable */
	prog_dir = dirname(strdup(argv[0]));
	snprintf(obj_path, sizeof(obj_path), "%s/sbd_kprobe.bpf.o", prog_dir);

	/* Initialize libbpf */
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
	libbpf_set_print(NULL);		/* Suppress libbpf debug output */

	/* Open and load BPF object */
	obj = bpf_object__open_file(obj_path, NULL);
	if (libbpf_get_error(obj)) {
		fprintf(stderr, "Failed to open BPF object: %s\n", obj_path);
		return 1;
	}

	if ((err = bpf_object__load(obj))) {
		fprintf(stderr, "Failed to load BPF object: %d\n", err);
		goto out;
	}

	/* Find kprobe and kretprobe programs */
	bpf_object__for_each_program(p, obj) {
		const char *sec = bpf_program__section_name(p);

		if (!sec)
			continue;

		if (!kp && strncmp(sec, "kprobe", 6) == 0)
			kp = p;
		else if (!krp && strncmp(sec, "kretprobe", 9) == 0)
			krp = p;
	}

	if (!kp || !krp) {
		fprintf(stderr, "Kprobe sections not found in BPF object\n");
		err = -1;
		goto out;
	}

	/* Attach kprobe for function entry */
	l1 = bpf_program__attach_kprobe(kp, false /* not retprobe */, func);
	if (libbpf_get_error(l1)) {
		fprintf(stderr, "Kprobe attach failed (func=%s)\n", func);
		err = -1;
		goto out;
	}

	/* Attach kretprobe for function exit */
	l2 = bpf_program__attach_kprobe(krp, true /* retprobe */, func);
	if (libbpf_get_error(l2)) {
		fprintf(stderr, "Kretprobe attach failed (func=%s)\n", func);
		err = -1;
		goto out;
	}

	/* Set up ring buffer for receiving events */
	struct bpf_map *evt_map = bpf_object__find_map_by_name(obj, "events");
	if (!evt_map) {
		fprintf(stderr, "events map not found\n");
		err = -1;
		goto out;
	}

	rb = ring_buffer__new(bpf_map__fd(evt_map), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		err = -1;
		goto out;
	}

	printf("Kprobes attached to %s(). Ctrl-C to stop.\n", func);

	/* Set up signal handler and poll for events */
	signal(SIGINT, on_sigint);

	while (!exiting) {
		err = ring_buffer__poll(rb, 250 /* ms timeout */);
		if (err == -EINTR)
			break;
		if (err < 0) {
			fprintf(stderr, "Ring buffer poll error: %d\n", err);
			break;
		}
	}

out:
	ring_buffer__free(rb);
	bpf_link__destroy(l1);
	bpf_link__destroy(l2);
	bpf_object__close(obj);
	return err ? 1 : 0;
}
