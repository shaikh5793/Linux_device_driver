/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_pipeline.c - Full capture → export → import → output pipeline test
 *
 * Capstone: ties together all V4L2 concepts from Parts 2-9:
 *   1. Open capture device (vcam_expbuf from Part 7)
 *   2. Set format, allocate MMAP buffers, start streaming
 *   3. Capture a frame, export it via VIDIOC_EXPBUF → dma-buf fd
 *   4. Pass the dma-buf fd to the output device (vout_dmabuf from Part 9)
 *   5. Output device imports and "displays" the frame
 *   6. Also pass to buf_reader (Part 8) for cross-device verification
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
#include <linux/ioctl.h>

#define BUF_READER_IOCTL_IMPORT _IOW('V', 1, int)

#define NUM_BUFFERS 4
#define WIDTH       640
#define HEIGHT      480

struct buffer_info {
	void   *start;
	size_t  length;
};

static int find_device(const char *driver_name)
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
		    strcmp((char *)cap.driver, driver_name) == 0) {
			printf("  Found %s at %s\n", driver_name, path);
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
	int cap_fd = -1, out_fd = -1, br_fd = -1, dmabuf_fd = -1;
	int i, pass = 1;

	printf("=== Part 10: Full V4L2 Pipeline Test ===\n");
	printf("=== capture → export → import → output ===\n\n");

	/* ---- Step 1: Open capture device ---- */
	printf("[1] Opening capture device (vcam_expbuf)...\n");
	cap_fd = find_device("vcam_expbuf");
	if (cap_fd < 0) {
		printf("FAIL: capture device not found\n");
		return 1;
	}

	/* ---- Step 2: Configure and start capture ---- */
	printf("[2] Configuring capture: %dx%d RGB24...\n", WIDTH, HEIGHT);
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(cap_fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("  S_FMT (capture)");
		pass = 0;
		goto cleanup;
	}

	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(cap_fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("  REQBUFS");
		pass = 0;
		goto cleanup;
	}

	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(cap_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("  QUERYBUF");
			pass = 0;
			goto cleanup;
		}
		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED, cap_fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			buffers[i].start = NULL;
			perror("  mmap");
			pass = 0;
			goto cleanup;
		}
	}

	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (ioctl(cap_fd, VIDIOC_QBUF, &buf) < 0) {
			perror("  QBUF");
			pass = 0;
			goto cleanup;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(cap_fd, VIDIOC_STREAMON, &type) < 0) {
		perror("  STREAMON");
		pass = 0;
		goto cleanup;
	}
	printf("  Capture streaming started\n");

	/* ---- Step 3: Capture a frame ---- */
	printf("[3] Capturing a frame...\n");
	pfd.fd = cap_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) <= 0) {
		printf("  FAIL: poll timeout\n");
		pass = 0;
		goto streamoff_cap;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(cap_fd, VIDIOC_DQBUF, &buf) < 0) {
		perror("  DQBUF");
		pass = 0;
		goto streamoff_cap;
	}

	unsigned char *data = buffers[buf.index].start;
	unsigned int frame_counter = *(unsigned int *)data;
	printf("  Captured: counter=%u, pixel@4: R=%u G=%u B=%u\n",
	       frame_counter, data[12], data[13], data[14]);

	/* ---- Step 4: Export as dma-buf ---- */
	printf("[4] Exporting buffer via VIDIOC_EXPBUF...\n");
	memset(&expbuf, 0, sizeof(expbuf));
	expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	expbuf.index = buf.index;
	expbuf.flags = O_RDONLY;
	if (ioctl(cap_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
		perror("  EXPBUF");
		pass = 0;
		goto requeue;
	}
	dmabuf_fd = expbuf.fd;
	printf("  Got dma-buf fd=%d\n", dmabuf_fd);

	/* ---- Step 5: Pass to buf_reader (cross-device verification) ---- */
	printf("[5] Passing to buf_reader for cross-device read...\n");
	br_fd = open("/dev/buf_reader", O_RDWR);
	if (br_fd < 0) {
		printf("  SKIP: buf_reader not loaded (optional)\n");
	} else {
		if (ioctl(br_fd, BUF_READER_IOCTL_IMPORT, &dmabuf_fd) < 0) {
			perror("  BUF_READER_IOCTL_IMPORT");
			printf("  WARN: buf_reader import failed\n");
		} else {
			printf("  OK: buf_reader verified frame (check dmesg)\n");
		}
		close(br_fd);
		br_fd = -1;
	}

	/* ---- Step 6: Import into output device ---- */
	printf("[6] Opening output device (vout_dmabuf)...\n");
	out_fd = find_device("vout_dmabuf");
	if (out_fd < 0) {
		printf("  SKIP: output device not loaded (optional)\n");
		goto done;
	}

	/* Set output format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width       = WIDTH;
	fmt.fmt.pix.height      = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(out_fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("  S_FMT (output)");
		pass = 0;
		goto done;
	}

	/* Request DMABUF buffer */
	memset(&req, 0, sizeof(req));
	req.count  = 1;
	req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(out_fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("  REQBUFS (output)");
		pass = 0;
		goto done;
	}

	/* Queue the exported dma-buf fd */
	printf("  Queuing dma-buf fd=%d to output...\n", dmabuf_fd);
	memset(&buf, 0, sizeof(buf));
	buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory    = V4L2_MEMORY_DMABUF;
	buf.index     = 0;
	buf.m.fd      = dmabuf_fd;
	buf.bytesused = fmt.fmt.pix.sizeimage;
	buf.length    = fmt.fmt.pix.sizeimage;
	if (ioctl(out_fd, VIDIOC_QBUF, &buf) < 0) {
		perror("  QBUF (output)");
		pass = 0;
		goto done;
	}

	/* Stream on output */
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(out_fd, VIDIOC_STREAMON, &type) < 0) {
		perror("  STREAMON (output)");
		pass = 0;
		goto done;
	}

	/* Wait for output processing */
	pfd.fd = out_fd;
	pfd.events = POLLOUT;
	poll(&pfd, 1, 5000);

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(out_fd, VIDIOC_DQBUF, &buf) < 0) {
		perror("  DQBUF (output)");
		pass = 0;
	} else {
		printf("  OK: output device processed the frame\n");
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(out_fd, VIDIOC_STREAMOFF, &type);

done:
	printf("\n[7] Pipeline complete — check dmesg for full trace\n");

requeue:
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);

streamoff_cap:
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(cap_fd, VIDIOC_STREAMOFF, &type);

cleanup:
	for (i = 0; i < NUM_BUFFERS; i++) {
		if (buffers[i].start && buffers[i].start != MAP_FAILED)
			munmap(buffers[i].start, buffers[i].length);
	}
	if (cap_fd >= 0)
		close(cap_fd);
	if (out_fd >= 0)
		close(out_fd);
	if (br_fd >= 0)
		close(br_fd);

	printf("\n=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
