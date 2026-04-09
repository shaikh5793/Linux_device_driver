/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * buffers.c - V4L2 buffer allocation and MMAP concepts
 *
 * Demonstrates the buffer lifecycle WITHOUT streaming — a prep
 * exercise for Part 3 where VB2 manages buffers in-kernel.
 *
 *   - VIDIOC_REQBUFS  — request buffer allocation (MMAP mode)
 *   - VIDIOC_QUERYBUF — query buffer metadata (offset, length)
 *   - mmap()          — map kernel buffers into userspace
 *   - munmap()        — release the mapping
 *   - VIDIOC_REQBUFS(0) — free all buffers
 *
 * This program allocates and inspects buffers but does NOT stream.
 * Streaming (QBUF/DQBUF/STREAMON) is covered in Part 3's test program.
 *
 * Usage: ./buffers /dev/video0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <linux/videodev2.h>

#define NUM_BUFFERS 4

/* Per-buffer tracking: mmap address, size, and driver-assigned offset */
struct buffer_info {
	void   *start;
	size_t  length;
	__u32   offset;
};

int main(int argc, char *argv[])
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct buffer_info buffers[NUM_BUFFERS] = { 0 };
	int fd, i;
	const char *device_path;

	if (argc != 2) {
		printf("Usage: %s /dev/videoN\n", argv[0]);
		return 1;
	}

	device_path = argv[1];
	printf("V4L2 Buffer Allocation & MMAP — %s\n", device_path);
	printf("======================================\n\n");

	fd = open(device_path, O_RDWR);
	if (fd < 0) {
		printf("Cannot open %s: %s\n", device_path, strerror(errno));
		return 1;
	}

	/* Verify device supports streaming */
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("QUERYCAP");
		close(fd);
		return 1;
	}
	if (!(cap.device_caps & V4L2_CAP_STREAMING)) {
		printf("Device does not support streaming I/O\n");
		close(fd);
		return 1;
	}
	printf("Device: %s (%s)\n\n", cap.card, cap.driver);

	/* Set format so buffer sizes are known */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		close(fd);
		return 1;
	}
	printf("[1] Format set: %dx%d, sizeimage=%u bytes\n\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

	/* Request buffer allocation in MMAP mode */
	printf("[2] Requesting %d buffers (VIDIOC_REQBUFS, MMAP mode)\n",
	       NUM_BUFFERS);
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("  REQBUFS");
		close(fd);
		return 1;
	}
	printf("  Driver allocated %u buffer(s)\n\n", req.count);

	/* Query each buffer's metadata, then mmap it into our address space */
	printf("[3] Querying and mapping buffers (VIDIOC_QUERYBUF + mmap)\n");
	for (i = 0; i < (int)req.count && i < NUM_BUFFERS; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("  QUERYBUF");
			break;
		}

		buffers[i].length = buf.length;
		buffers[i].offset = buf.m.offset;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, buf.m.offset);

		if (buffers[i].start == MAP_FAILED) {
			buffers[i].start = NULL;
			perror("  mmap");
			break;
		}

		printf("  Buffer %d: length=%u, offset=0x%x, mapped at %p\n",
		       i, buf.length, buf.m.offset, buffers[i].start);
	}
	printf("\n");

	printf("[4] Buffer summary\n");
	printf("  %d buffers allocated, each %u bytes\n",
	       (int)req.count, fmt.fmt.pix.sizeimage);
	printf("  Total kernel memory: %u bytes\n",
	       req.count * fmt.fmt.pix.sizeimage);
	printf("  All mapped into userspace — ready for streaming\n\n");

	/* Cleanup: munmap all buffers, then free them with REQBUFS(0) */
	printf("[5] Cleaning up (munmap + REQBUFS(0))\n");
	for (i = 0; i < (int)req.count && i < NUM_BUFFERS; i++) {
		if (buffers[i].start) {
			munmap(buffers[i].start, buffers[i].length);
			printf("  Buffer %d unmapped\n", i);
		}
	}

	/* Free all buffers by requesting 0 */
	memset(&req, 0, sizeof(req));
	req.count  = 0;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(fd, VIDIOC_REQBUFS, &req);
	printf("  Buffers freed\n");

	close(fd);
	printf("\nDone.\n");
	return 0;
}
