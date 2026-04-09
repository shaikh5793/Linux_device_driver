// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Capture Test — Part 6
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Userspace test program: opens the bridge video device, sets format,
 * allocates MMAP buffers, streams 5 frames, and prints frame info.
 *
 * Usage: ./test_capture [/dev/videoN]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NUM_BUFFERS	4
#define NUM_FRAMES	5
#define DEFAULT_DEV	"/dev/video0"

struct buffer_info {
	void *start;
	size_t length;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

int main(int argc, char *argv[])
{
	const char *dev_path = (argc > 1) ? argv[1] : DEFAULT_DEV;
	struct buffer_info buffers[NUM_BUFFERS];
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	enum v4l2_buf_type type;
	int fd, i, ret;

	printf("=== VSOC-3000 Part 6: Capture Test ===\n\n");

	/* Open device */
	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		perror("open");
		fprintf(stderr, "Failed to open %s\n", dev_path);
		return 1;
	}
	printf("Opened %s (fd=%d)\n", dev_path, fd);

	/* QUERYCAP */
	memset(&cap, 0, sizeof(cap));
	ret = xioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		perror("VIDIOC_QUERYCAP");
		goto out_close;
	}

	printf("Driver:   %s\n", cap.driver);
	printf("Card:     %s\n", cap.card);
	printf("Bus:      %s\n", cap.bus_info);
	printf("Caps:     0x%08x\n", cap.device_caps);

	/* Verify capabilities */
	if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "ERROR: not a video capture device\n");
		ret = 1;
		goto out_close;
	}
	if (!(cap.device_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "ERROR: streaming not supported\n");
		ret = 1;
		goto out_close;
	}
	printf("  VIDEO_CAPTURE: YES\n");
	printf("  STREAMING:     YES\n\n");

	/* S_FMT: 640x480 RGB24 */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	ret = xioctl(fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		perror("VIDIOC_S_FMT");
		goto out_close;
	}

	printf("Format set: %ux%u, pixfmt=%.4s, sizeimage=%u\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       (char *)&fmt.fmt.pix.pixelformat,
	       fmt.fmt.pix.sizeimage);

	/* REQBUFS: request 4 MMAP buffers */
	memset(&req, 0, sizeof(req));
	req.count = NUM_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	ret = xioctl(fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		perror("VIDIOC_REQBUFS");
		goto out_close;
	}
	printf("Allocated %u MMAP buffers\n", req.count);

	/* QUERYBUF + mmap each buffer */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		ret = xioctl(fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QUERYBUF");
			goto out_unmap;
		}

		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			goto out_unmap;
		}
		printf("  Buffer %d: length=%zu, mapped=%p\n",
		       i, buffers[i].length, buffers[i].start);
	}

	/* QBUF all buffers */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		ret = xioctl(fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QBUF");
			goto out_unmap;
		}
	}

	/* STREAMON */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = xioctl(fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		perror("VIDIOC_STREAMON");
		goto out_unmap;
	}
	printf("\nStreaming ON\n\n");

	/* DQBUF loop: dequeue 5 frames */
	for (i = 0; i < NUM_FRAMES; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		ret = xioctl(fd, VIDIOC_DQBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}

		/* Print frame info */
		unsigned char *data = buffers[buf.index].start;

		printf("Frame %d: seq=%u, ts=%lu.%06lu, bytes=%u, "
		       "first4=[%02x %02x %02x %02x]\n",
		       i, buf.sequence,
		       (unsigned long)buf.timestamp.tv_sec,
		       (unsigned long)buf.timestamp.tv_usec,
		       buf.bytesused,
		       data[0], data[1], data[2], data[3]);

		/* Re-queue buffer */
		ret = xioctl(fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QBUF (re-queue)");
			break;
		}
	}

	/* STREAMOFF */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = xioctl(fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
		perror("VIDIOC_STREAMOFF");
	printf("\nStreaming OFF\n");

	ret = 0;

out_unmap:
	for (i = 0; i < NUM_BUFFERS; i++) {
		if (buffers[i].start && buffers[i].start != MAP_FAILED)
			munmap(buffers[i].start, buffers[i].length);
	}
out_close:
	close(fd);

	printf("\n=== Test complete ===\n");
	return ret;
}
