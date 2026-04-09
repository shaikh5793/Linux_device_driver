/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_import.c - DMA heap alloc → V4L2 output via DMABUF
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <stdint.h>

/* DMA heap userspace interface */
struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

#define WIDTH   640
#define HEIGHT  480
#define BPP     3
#define BUFSIZE (WIDTH * HEIGHT * BPP)

static int find_vout_device(void)
{
	char path[32];
	struct v4l2_capability cap;
	int fd, i;

	for (i = 0; i < 10; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
		    strcmp((char *)cap.driver, "vout_dmabuf") == 0) {
			printf("Found vout_dmabuf at %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

static int alloc_dma_heap(int *dmabuf_fd)
{
	struct dma_heap_allocation_data alloc;
	int heap_fd;

	heap_fd = open("/dev/dma_heap/system", O_RDONLY);
	if (heap_fd < 0) {
		heap_fd = open("/dev/dma_heap/system-uncached", O_RDONLY);
		if (heap_fd < 0) {
			perror("open dma_heap");
			return -1;
		}
		printf("Using system-uncached heap\n");
	} else {
		printf("Using system heap\n");
	}

	memset(&alloc, 0, sizeof(alloc));
	alloc.len      = BUFSIZE;
	alloc.fd_flags = O_RDWR | O_CLOEXEC;

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC");
		close(heap_fd);
		return -1;
	}

	close(heap_fd);
	*dmabuf_fd = alloc.fd;
	printf("DMA heap allocated %u bytes, fd=%u\n", BUFSIZE, alloc.fd);
	return 0;
}

int main(void)
{
	int dmabuf_fd = -1, vfd = -1;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct pollfd pfd;
	enum v4l2_buf_type type;
	unsigned char *mapped;
	unsigned int i;
	int pass = 1;

	printf("=== vout_dmabuf Import Test ===\n\n");

	/* Step 1: Allocate from DMA heap */
	printf("[1] Allocating from DMA heap...\n");
	if (alloc_dma_heap(&dmabuf_fd) < 0) {
		printf("FAIL: DMA heap allocation failed\n");
		printf("(is the system heap available? check /dev/dma_heap/)\n");
		return 1;
	}

	/* Step 2: Write test pattern */
	printf("[2] Writing test pattern...\n");
	mapped = mmap(NULL, BUFSIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		      dmabuf_fd, 0);
	if (mapped == MAP_FAILED) {
		perror("mmap dma_buf");
		close(dmabuf_fd);
		return 1;
	}

	/* Marker in first 4 bytes */
	*(uint32_t *)mapped = 0xDEADBEEF;
	/* RGB24 pattern: B channel = 0xAB */
	for (i = 4; i < BUFSIZE; i += 3) {
		mapped[i]   = (i / 3) & 0xff;  /* R */
		mapped[i+1] = 0x42;             /* G */
		if (i + 2 < BUFSIZE)
			mapped[i+2] = 0xAB;     /* B = marker */
	}
	munmap(mapped, BUFSIZE);
	printf("  Marker=0xDEADBEEF, B=0xAB pattern written\n\n");

	/* Step 3: Find V4L2 output device */
	printf("[3] Opening V4L2 output device...\n");
	vfd = find_vout_device();
	if (vfd < 0) {
		printf("FAIL: vout_dmabuf device not found\n");
		close(dmabuf_fd);
		return 1;
	}

	/* Step 4: Set format */
	printf("[4] Setting format...\n");
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width       = WIDTH;
	fmt.fmt.pix.height      = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		pass = 0;
		goto cleanup;
	}
	printf("  Format: %ux%u RGB24\n\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

	/* Step 5: REQBUFS with DMABUF */
	printf("[5] Requesting DMABUF buffer...\n");
	memset(&req, 0, sizeof(req));
	req.count  = 1;
	req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		pass = 0;
		goto cleanup;
	}
	printf("  %u buffer(s) allocated\n\n", req.count);

	/* Step 6: QBUF with dma-buf fd */
	printf("[6] Queuing dma_buf fd=%d...\n", dmabuf_fd);
	memset(&buf, 0, sizeof(buf));
	buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory   = V4L2_MEMORY_DMABUF;
	buf.index    = 0;
	buf.m.fd     = dmabuf_fd;
	buf.bytesused = BUFSIZE;
	buf.length   = BUFSIZE;
	if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
		perror("QBUF");
		pass = 0;
		goto cleanup;
	}

	/* Step 7: STREAMON */
	printf("[7] Stream ON...\n");
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		pass = 0;
		goto cleanup;
	}

	/* Wait for processing */
	pfd.fd = vfd;
	pfd.events = POLLOUT;
	if (poll(&pfd, 1, 5000) <= 0) {
		printf("WARN: poll timeout\n");
	}

	/* DQBUF */
	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
		perror("DQBUF");
		pass = 0;
	} else {
		printf("  Buffer processed by driver\n");
	}

	/* STREAMOFF */
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(vfd, VIDIOC_STREAMOFF, &type);
	printf("\n  Check dmesg — should show marker=0xDEADBEEF\n");

cleanup:
	if (vfd >= 0)
		close(vfd);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);

	printf("\n=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
