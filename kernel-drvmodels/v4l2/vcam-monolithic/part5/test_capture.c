/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_capture.c - Userspace MMAP capture test for vcam_irq
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

#define NUM_BUFFERS  4
#define NUM_FRAMES   5

struct buffer_info {
	void   *start;
	size_t  length;
};

static int find_device(void)
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
		    strcmp((char *)cap.driver, "vcam_irq") == 0) {
			printf("Found vcam_irq at %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(void)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct buffer_info buffers[NUM_BUFFERS];
	struct pollfd pfd;
	enum v4l2_buf_type type;
	int fd, i, pass = 1;

	printf("=== vcam_irq Capture Test ===\n\n");

	fd = find_device();
	if (fd < 0) {
		printf("FAIL: vcam_irq device not found\n");
		return 1;
	}

	/* Set format */
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
	printf("Format: %ux%u RGB24 (sizeimage=%u)\n\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

	/* Request buffers */
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		close(fd);
		return 1;
	}
	printf("Allocated %u buffers\n", req.count);

	/* Query and mmap each buffer */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("QUERYBUF");
			pass = 0;
			goto cleanup;
		}
		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			pass = 0;
			goto cleanup;
		}
	}

	/* Queue all buffers */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			pass = 0;
			goto cleanup;
		}
	}

	/* Start streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		pass = 0;
		goto cleanup;
	}
	printf("Streaming started\n\n");

	/* Capture frames */
	for (i = 0; i < NUM_FRAMES; i++) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 5000) <= 0) {
			printf("FAIL: poll timeout on frame %d\n", i);
			pass = 0;
			break;
		}

		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			perror("DQBUF");
			pass = 0;
			break;
		}

		/* Read frame data */
		unsigned char *data = buffers[buf.index].start;
		unsigned int frame_counter = *(unsigned int *)data;
		unsigned char r = data[12], g = data[13], b = data[14];

		printf("Frame %d: seq=%u, counter=%u, pixel@4: R=%u G=%u B=%u\n",
		       i, buf.sequence, frame_counter, r, g, b);

		/* Re-queue */
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF requeue");
			pass = 0;
			break;
		}
	}

	/* Stop streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);
	printf("\nStreaming stopped\n");

cleanup:
	for (i = 0; i < (int)req.count; i++) {
		if (buffers[i].start && buffers[i].start != MAP_FAILED)
			munmap(buffers[i].start, buffers[i].length);
	}
	close(fd);

	printf("\n=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
