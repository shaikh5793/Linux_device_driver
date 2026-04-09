/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_expbuf.c - Test VIDIOC_EXPBUF + buf_reader import
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

/* Duplicate the ioctl define for userspace */
#include <linux/ioctl.h>
#define BUF_READER_IOCTL_IMPORT _IOW('V', 1, int)

#define NUM_BUFFERS 4

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
		    strcmp((char *)cap.driver, "vcam_expbuf") == 0) {
			printf("Found vcam_expbuf at %s\n", path);
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
	struct v4l2_exportbuffer expbuf;
	struct buffer_info buffers[NUM_BUFFERS];
	struct pollfd pfd;
	enum v4l2_buf_type type;
	int vfd, brfd, dmabuf_fd;
	int i, pass = 1;

	printf("=== vcam_expbuf + buf_reader Test ===\n\n");

	vfd = find_device();
	if (vfd < 0) {
		printf("FAIL: vcam_expbuf device not found\n");
		return 1;
	}

	/* Set format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		close(vfd);
		return 1;
	}
	printf("Format: %ux%u RGB24\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

	/* Request buffers */
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		close(vfd);
		return 1;
	}

	/* Query + mmap each buffer */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("QUERYBUF");
			pass = 0;
			goto cleanup;
		}
		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED, vfd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			pass = 0;
			goto cleanup;
		}
	}

	/* Queue all */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			pass = 0;
			goto cleanup;
		}
	}

	/* Start streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		pass = 0;
		goto cleanup;
	}
	printf("Streaming started\n");

	/* Capture one frame */
	pfd.fd = vfd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) <= 0) {
		printf("FAIL: poll timeout\n");
		pass = 0;
		goto streamoff;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
		perror("DQBUF");
		pass = 0;
		goto streamoff;
	}

	/* Read frame data from mmap */
	unsigned char *data = buffers[buf.index].start;
	unsigned int frame_counter = *(unsigned int *)data;
	printf("Captured frame: counter=%u, pixel@4: R=%u G=%u B=%u\n",
	       frame_counter, data[12], data[13], data[14]);

	/* EXPBUF — export this buffer as a dma-buf fd */
	memset(&expbuf, 0, sizeof(expbuf));
	expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	expbuf.index = buf.index;
	expbuf.flags = O_RDONLY;
	if (ioctl(vfd, VIDIOC_EXPBUF, &expbuf) < 0) {
		perror("EXPBUF");
		pass = 0;
		goto requeue;
	}
	dmabuf_fd = expbuf.fd;
	printf("EXPBUF: got dma_buf fd=%d\n", dmabuf_fd);

	/* Pass to buf_reader */
	brfd = open("/dev/buf_reader", O_RDWR);
	if (brfd < 0) {
		perror("open /dev/buf_reader");
		printf("(is buf_reader.ko loaded?)\n");
		close(dmabuf_fd);
		pass = 0;
		goto requeue;
	}

	if (ioctl(brfd, BUF_READER_IOCTL_IMPORT, &dmabuf_fd) < 0) {
		perror("BUF_READER_IOCTL_IMPORT");
		pass = 0;
	} else {
		printf("buf_reader import OK — check dmesg for pixel values\n");
	}

	close(brfd);
	close(dmabuf_fd);

requeue:
	/* Re-queue the buffer */
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	ioctl(vfd, VIDIOC_QBUF, &buf);

streamoff:
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(vfd, VIDIOC_STREAMOFF, &type);

cleanup:
	for (i = 0; i < (int)req.count; i++) {
		if (buffers[i].start && buffers[i].start != MAP_FAILED)
			munmap(buffers[i].start, buffers[i].length);
	}
	close(vfd);

	printf("\n=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
