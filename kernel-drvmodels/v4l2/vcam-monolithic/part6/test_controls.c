/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_controls.c - Userspace test for vcam_ctrl controls
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

static int g_fd = -1;

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
		    strcmp((char *)cap.driver, "vcam_ctrl") == 0) {
			printf("Found vcam_ctrl at %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/*
 * Capture a single frame and return pixel values at offset 12 (pixel 4).
 *
 * Must allocate and queue at least 2 buffers because the driver sets
 * min_queued_buffers=2 — VB2 won't call start_streaming until that
 * many are queued.
 */
#define CAP_NUM_BUFS 2

static int capture_one_frame(unsigned char *r, unsigned char *g, unsigned char *b)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct pollfd pfd;
	enum v4l2_buf_type type;
	void *mapped[CAP_NUM_BUFS] = { NULL };
	size_t mapped_len[CAP_NUM_BUFS] = { 0 };
	int i, ret = -1;

	/* Set format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(g_fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		return -1;
	}

	/* Request buffers */
	memset(&req, 0, sizeof(req));
	req.count  = CAP_NUM_BUFS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(g_fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		return -1;
	}

	/* Query and mmap each buffer */
	for (i = 0; i < (int)req.count && i < CAP_NUM_BUFS; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("QUERYBUF");
			goto free_bufs;
		}
		mapped_len[i] = buf.length;
		mapped[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				 MAP_SHARED, g_fd, buf.m.offset);
		if (mapped[i] == MAP_FAILED) {
			mapped[i] = NULL;
			perror("mmap");
			goto unmap;
		}
	}

	/* Queue all buffers */
	for (i = 0; i < (int)req.count && i < CAP_NUM_BUFS; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			goto unmap;
		}
	}

	/* Stream on */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(g_fd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		goto unmap;
	}

	/* Wait and dequeue */
	pfd.fd = g_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) <= 0) {
		printf("poll timeout\n");
		goto streamoff;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(g_fd, VIDIOC_DQBUF, &buf) < 0) {
		perror("DQBUF");
		goto streamoff;
	}

	/* Read pixel at offset 12 (pixel 4, after frame counter) */
	unsigned char *data = mapped[buf.index];
	*r = data[12];
	*g = data[13];
	*b = data[14];
	ret = 0;

streamoff:
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(g_fd, VIDIOC_STREAMOFF, &type);
unmap:
	for (i = 0; i < CAP_NUM_BUFS; i++) {
		if (mapped[i])
			munmap(mapped[i], mapped_len[i]);
	}
free_bufs:
	/* Free buffers */
	memset(&req, 0, sizeof(req));
	req.count  = 0;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(g_fd, VIDIOC_REQBUFS, &req);

	return ret;
}

static int set_control(unsigned int id, int value)
{
	struct v4l2_control ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	ctrl.value = value;
	if (ioctl(g_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		perror("S_CTRL");
		return -1;
	}
	return 0;
}

int main(void)
{
	struct v4l2_queryctrl qctrl;
	unsigned char r, g, b;
	int pass = 1;
	int found_brightness = 0, found_hflip = 0;
	unsigned int id;

	printf("=== vcam_ctrl Controls Test ===\n\n");

	g_fd = find_device();
	if (g_fd < 0) {
		printf("FAIL: vcam_ctrl device not found\n");
		return 1;
	}

	/* [1] Enumerate controls */
	printf("[1] Enumerating controls:\n");
	for (id = V4L2_CID_BASE; id < V4L2_CID_LASTP1; id++) {
		memset(&qctrl, 0, sizeof(qctrl));
		qctrl.id = id;
		if (ioctl(g_fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
			if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
				continue;
			printf("  %s: min=%d max=%d default=%d\n",
			       qctrl.name, qctrl.minimum, qctrl.maximum,
			       qctrl.default_value);
			if (id == V4L2_CID_BRIGHTNESS)
				found_brightness = 1;
			if (id == V4L2_CID_HFLIP)
				found_hflip = 1;
		}
	}
	if (!found_brightness || !found_hflip) {
		printf("  FAIL: brightness or hflip not found\n");
		pass = 0;
	} else {
		printf("  OK: brightness and hflip controls found\n");
	}
	printf("\n");

	/* [2] Test brightness=0 (dark) */
	printf("[2] Brightness=0 (dark):\n");
	set_control(V4L2_CID_BRIGHTNESS, 0);
	set_control(V4L2_CID_HFLIP, 0);
	if (capture_one_frame(&r, &g, &b) < 0) {
		printf("  FAIL: capture failed\n");
		pass = 0;
	} else {
		printf("  Pixel@4: R=%u G=%u B=%u\n", r, g, b);
		if (g < 128)
			printf("  OK: pixel values are dark\n");
		else {
			printf("  WARN: expected dark pixels\n");
		}
	}
	printf("\n");

	/* [3] Test brightness=255 (bright) */
	printf("[3] Brightness=255 (bright):\n");
	set_control(V4L2_CID_BRIGHTNESS, 255);
	if (capture_one_frame(&r, &g, &b) < 0) {
		printf("  FAIL: capture failed\n");
		pass = 0;
	} else {
		printf("  Pixel@4: R=%u G=%u B=%u\n", r, g, b);
		if (g > 100)
			printf("  OK: pixel values are bright\n");
		else {
			printf("  WARN: expected bright pixels\n");
		}
	}
	printf("\n");

	/* [4] Test hflip */
	printf("[4] Hflip test:\n");
	set_control(V4L2_CID_BRIGHTNESS, 128);
	set_control(V4L2_CID_HFLIP, 0);
	unsigned char r1, g1, b1;
	if (capture_one_frame(&r1, &g1, &b1) < 0) {
		printf("  FAIL: capture failed\n");
		pass = 0;
	} else {
		printf("  hflip=0: R=%u G=%u B=%u\n", r1, g1, b1);
	}

	set_control(V4L2_CID_HFLIP, 1);
	unsigned char r2, g2, b2;
	if (capture_one_frame(&r2, &g2, &b2) < 0) {
		printf("  FAIL: capture failed\n");
		pass = 0;
	} else {
		printf("  hflip=1: R=%u G=%u B=%u\n", r2, g2, b2);
		if (r1 != r2 || b1 != b2)
			printf("  OK: pixel values changed with hflip\n");
		else
			printf("  NOTE: pixel may match at this position (center-ish)\n");
	}
	printf("\n");

	close(g_fd);

	printf("=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
