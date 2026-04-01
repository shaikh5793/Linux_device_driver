/*
 * Copyright (c) 2024 TECH VEDA(www.techveda.org)
 * Author: Raghu Bharadwaj
 *
 * This software is licensed under GPL v2.
 * See the accompanying LICENSE file for the full text.
 *
 * Block Device I/O Tracer - Userspace Program
 *
 * This program uses eBPF tracepoints to monitor block layer I/O operations
 * for a specific block device. It traces:
 * - Request insertion (issue)
 * - Request completion
 * - Bio merge events (frontmerge/backmerge)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* Device filter configuration structure */
struct dev_filter_t {
	uint32_t major;		/* Device major number */
	uint32_t minor;		/* Device minor number */
};

/* Event types for block I/O tracing */
enum event_type {
	EV_ISSUE = 1,		/* Request issued to device */
	EV_DONE = 2,		/* Request completed */
	EV_MERGE_BACK = 3,	/* Bio merged at end of request */
	EV_MERGE_FRONT = 4,	/* Bio merged at beginning of request */
};

/* Event structure received from kernel BPF program */
struct event_t {
	uint64_t ts_ns;		/* Timestamp in nanoseconds */
	uint32_t type;		/* Event type (see enum above) */
	uint32_t pid;		/* Process ID */
	uint32_t major;		/* Device major number */
	uint32_t minor;		/* Device minor number */
	uint64_t sector;	/* Starting sector (LBA) */
	uint32_t bytes;		/* Number of bytes */
	int32_t err;		/* Error code (for completion events) */
	char rwbs[8];		/* Read/Write operation flags */
	char comm[16];		/* Process name */
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
 * @data_sz: Size of event data
 * @return: 0 on success
 */
static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event_t *e = data;
	const char *etype = "?";

	/* Convert event type to string for display */
	switch (e->type) {
	case EV_ISSUE:
		etype = "ISSUE";
		break;
	case EV_DONE:
		etype = "DONE";
		break;
	case EV_MERGE_BACK:
		etype = "MERGEb";
		break;
	case EV_MERGE_FRONT:
		etype = "MERGEf";
		break;
	}

	/* Print formatted event information */
	printf("%-6s op=%-4s lba=%-8llu bytes=%-6u err=%-2d pid=%-6u comm=%s\n",
	       etype, e->rwbs, (unsigned long long)e->sector, e->bytes,
	       e->err, e->pid, e->comm);

	return 0;
}

/*
 * Ring buffer lost event handler
 * Called when events are dropped due to buffer overflow
 *
 * @ctx: User context (unused)
 * @cpu: CPU number where events were lost
 * @cnt: Number of lost events
 * @return: 0
 */
static int handle_lost(void *ctx, int cpu, long long cnt)
{
	fprintf(stderr, "Lost %lld events on CPU %d\n", cnt, cpu);
	return 0;
}

/*
 * Extract device major/minor numbers from device path
 *
 * @path: Device path (e.g., /dev/sbd)
 * @maj: Pointer to store major number
 * @min: Pointer to store minor number
 * @return: 0 on success, -1 on error
 */
static int dev_from_path(const char *path, uint32_t *maj, uint32_t *min)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		perror("stat");
		return -1;
	}

	if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s is not a block device\n", path);
		return -1;
	}

	*maj = major(st.st_rdev);
	*min = minor(st.st_rdev);

	return 0;
}

int main(int argc, char **argv)
{
	const char *devpath = argc > 1 ? argv[1] : "/dev/sbd";
	uint32_t maj = 0, min = 0;
	struct bpf_object *obj = NULL;
	struct ring_buffer *rb = NULL;
	int err = 0, map_fd;
	char obj_path[1024];
	char *prog_dir;

	/* Find BPF object in same directory as executable */
	prog_dir = dirname(strdup(argv[0]));
	snprintf(obj_path, sizeof(obj_path), "%s/sbd_trace.bpf.o", prog_dir);

	/* Initialize libbpf */
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
	libbpf_set_print(NULL);		/* Suppress libbpf debug output */

	/* Get device major/minor numbers from path */
	if (dev_from_path(devpath, &maj, &min) < 0)
		return 1;

	/* Open and load BPF object */
	obj = bpf_object__open_file(obj_path, NULL);
	if (libbpf_get_error(obj)) {
		fprintf(stderr, "Failed to open BPF object: %s\n", obj_path);
		return 1;
	}

	if ((err = bpf_object__load(obj))) {
		fprintf(stderr, "Failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	/* Configure device filter in BPF map */
	struct bpf_map *cfg_map = bpf_object__find_map_by_name(obj, "device_filter");
	if (!cfg_map) {
		fprintf(stderr, "device_filter map not found\n");
		err = -1;
		goto cleanup;
	}

	map_fd = bpf_map__fd(cfg_map);
	struct dev_filter_t cfg = {.major = maj, .minor = min};
	uint32_t key = 0;

	if ((err = bpf_map_update_elem(map_fd, &key, &cfg, BPF_ANY)) < 0) {
		fprintf(stderr, "Failed to update filter map: %d\n", err);
		goto cleanup;
	}

	/* Attach all tracepoint programs */
	struct bpf_program *prog;
	bpf_object__for_each_program(prog, obj) {
		const char *sec = bpf_program__section_name(prog);

		if (strncmp(sec, "tracepoint/", 11) == 0) {
			struct bpf_link *link;
			const char *tp_category = NULL;
			const char *tp_name = NULL;
			char sec_copy[256];

			/* Parse tracepoint category and name from section */
			strncpy(sec_copy, sec + 11, sizeof(sec_copy) - 1);
			sec_copy[sizeof(sec_copy) - 1] = '\0';

			tp_category = sec_copy;
			tp_name = strchr(sec_copy, '/');
			if (tp_name) {
				*((char *)tp_name) = '\0';
				tp_name++;
			}

			link = bpf_program__attach_tracepoint(prog, tp_category, tp_name);
			if (libbpf_get_error(link)) {
				fprintf(stderr, "Attach failed for %s/%s (from %s)\n",
					tp_category, tp_name, sec);
				err = -1;
				goto cleanup;
			}
			printf("Attached tracepoint: %s/%s\n", tp_category, tp_name);
		}
	}

	/* Set up ring buffer for receiving events */
	struct bpf_map *evt_map = bpf_object__find_map_by_name(obj, "events");
	if (!evt_map) {
		fprintf(stderr, "events map not found\n");
		err = -1;
		goto cleanup;
	}

	rb = ring_buffer__new(bpf_map__fd(evt_map), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		err = -1;
		goto cleanup;
	}

	printf("Tracing %s (major=%u minor=%u). Ctrl-C to stop.\n",
	       devpath, maj, min);

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

cleanup:
	ring_buffer__free(rb);
	bpf_object__close(obj);
	return err ? 1 : 0;
}
