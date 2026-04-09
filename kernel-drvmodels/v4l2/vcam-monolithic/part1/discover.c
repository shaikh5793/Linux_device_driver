/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * discover.c - V4L2 device discovery and capability query
 *
 * Demonstrates:
 *   - Scanning /dev/videoN for V4L2 devices
 *   - VIDIOC_QUERYCAP ioctl
 *   - Decoding device_caps bitmask
 *   - Classifying devices by type
 *
 * Usage:
 *   ./discover              — scan all devices
 *   ./discover /dev/video0  — query one specific device
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/videodev2.h>

#define MAX_DEVICES 64

/* Decode a V4L2 capability bitmask into human-readable names. */
static void print_caps(unsigned int caps)
{
	struct { unsigned int bit; const char *name; } cap_table[] = {
		{ V4L2_CAP_VIDEO_CAPTURE,     "VIDEO_CAPTURE" },
		{ V4L2_CAP_VIDEO_OUTPUT,      "VIDEO_OUTPUT" },
		{ V4L2_CAP_VIDEO_OVERLAY,     "VIDEO_OVERLAY" },
		{ V4L2_CAP_VIDEO_CAPTURE_MPLANE, "VIDEO_CAPTURE_MPLANE" },
		{ V4L2_CAP_VIDEO_OUTPUT_MPLANE,  "VIDEO_OUTPUT_MPLANE" },
		{ V4L2_CAP_VIDEO_M2M,        "VIDEO_M2M" },
		{ V4L2_CAP_VIDEO_M2M_MPLANE, "VIDEO_M2M_MPLANE" },
		{ V4L2_CAP_META_CAPTURE,     "META_CAPTURE" },
		{ V4L2_CAP_STREAMING,        "STREAMING" },
		{ V4L2_CAP_READWRITE,        "READWRITE" },
		{ V4L2_CAP_IO_MC,            "IO_MC" },
	};
	int n = sizeof(cap_table) / sizeof(cap_table[0]);
	int first = 1;

	for (int i = 0; i < n; i++) {
		if (caps & cap_table[i].bit) {
			printf("%s%s", first ? "" : ", ", cap_table[i].name);
			first = 0;
		}
	}
	if (first)
		printf("(none)");
}

/* Open a V4L2 device node and print its capabilities. */
static void query_device(const char *path)
{
	struct v4l2_capability cap;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("  Cannot open: %s\n", strerror(errno));
		return;
	}

	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		printf("  QUERYCAP failed: %s\n", strerror(errno));
		close(fd);
		return;
	}

	printf("  Driver:      %s\n", cap.driver);
	printf("  Card:        %s\n", cap.card);
	printf("  Bus:         %s\n", cap.bus_info);
	printf("  Version:     %d.%d.%d\n",
	       (cap.version >> 16) & 0xff,
	       (cap.version >> 8) & 0xff,
	       cap.version & 0xff);

	/* Use device_caps (not capabilities) for this specific node's features. */
	printf("  Device caps: 0x%08x [", cap.device_caps);
	print_caps(cap.device_caps);
	printf("]\n");

	printf("  Type:        ");
	if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)
		printf("Capture ");
	if (cap.device_caps & V4L2_CAP_VIDEO_OUTPUT)
		printf("Output ");
	if (cap.device_caps & V4L2_CAP_VIDEO_M2M)
		printf("M2M ");
	if (cap.device_caps & V4L2_CAP_META_CAPTURE)
		printf("Metadata ");
	printf("\n");

	close(fd);
}

/* Discover all V4L2 devices by scanning /dev/video0..63. */
static int scan_all_devices(void)
{
	char path[32];
	struct v4l2_capability cap;
	int fd, count = 0;

	printf("Scanning for V4L2 devices...\n\n");

	for (int i = 0; i < MAX_DEVICES; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);

		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			printf("%s:\n", path);
			close(fd);
			/*
			 * Re-open inside query_device() so it can also be
			 * called standalone from main().
			 */
			query_device(path);
			printf("\n");
			count++;
		} else {
			close(fd);
		}
	}

	return count;
}

/* Entry point: scan all devices or query a specific one. */
int main(int argc, char *argv[])
{
	printf("V4L2 Device Discovery & Capabilities\n");
	printf("=====================================\n\n");

	if (argc == 2) {
		printf("%s:\n", argv[1]);
		query_device(argv[1]);
		return 0;
	}

	int count = scan_all_devices();

	printf("Found %d V4L2 device(s)\n", count);

	if (count == 0) {
		printf("\nNo V4L2 devices found. You may need to:\n");
		printf("  1. Connect a USB camera\n");
		printf("  2. Load a virtual device: sudo modprobe v4l2loopback\n");
		printf("  3. Check permissions (add user to 'video' group)\n");
		return 1;
	}

	return 0;
}
