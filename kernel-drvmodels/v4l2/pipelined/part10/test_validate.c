// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Pipeline Validation Test -- Part 10
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Userspace test that exercises media pipeline validation:
 *
 *   Test 1: Set matching formats on sensor and CSI-2, try to stream
 *           -> should SUCCEED
 *
 *   Test 2: Set mismatched format on sensor (different resolution),
 *           try to stream again
 *           -> should FAIL with EPIPE (pipeline validation error)
 *
 * Usage: ./test_validate [/dev/video0 [/dev/v4l-subdev0 [/dev/v4l-subdev1]]]
 *
 * The test opens the video device and subdev device nodes to set formats,
 * then attempts to stream via VIDIOC_STREAMON.
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
#include <linux/v4l2-subdev.h>

#define DEFAULT_VIDEO_DEV	"/dev/video0"
#define DEFAULT_SENSOR_DEV	"/dev/v4l-subdev0"
#define DEFAULT_CSI2_DEV	"/dev/v4l-subdev1"

#define NUM_BUFFERS		2

/* Helper: set subdev format on a given pad */
static int set_subdev_fmt(int fd, const char *name, unsigned int pad,
			  unsigned int width, unsigned int height,
			  unsigned int code)
{
	struct v4l2_subdev_format fmt;
	int ret;

	memset(&fmt, 0, sizeof(fmt));
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad   = pad;
	fmt.format.width      = width;
	fmt.format.height     = height;
	fmt.format.code       = code;
	fmt.format.field      = V4L2_FIELD_NONE;
	fmt.format.colorspace = V4L2_COLORSPACE_RAW;

	ret = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt);
	if (ret < 0) {
		printf("  [%s] VIDIOC_SUBDEV_S_FMT pad=%u failed: %s\n",
		       name, pad, strerror(errno));
		return -1;
	}

	printf("  [%s] pad=%u format set to %ux%u code=0x%04x\n",
	       name, pad, fmt.format.width, fmt.format.height,
	       fmt.format.code);
	return 0;
}

/* Helper: set video device format */
static int set_video_fmt(int fd, unsigned int width, unsigned int height)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = width;
	fmt.fmt.pix.height      = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;

	ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("  [video] VIDIOC_S_FMT failed: %s\n",
		       strerror(errno));
		return -1;
	}

	printf("  [video] format set to %ux%u\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height);
	return 0;
}

/* Helper: request buffers, queue them, and try to start streaming */
static int try_stream(int fd, const char *label)
{
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	enum v4l2_buf_type type;
	int ret;
	unsigned int i;

	printf("\n  --- %s: attempting to stream ---\n", label);

	/* Request buffers */
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		printf("  VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		return -1;
	}

	/* Queue buffers */
	for (i = 0; i < req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;

		ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			printf("  VIDIOC_QUERYBUF %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}

		/* mmap the buffer (required for MMAP streaming) */
		void *ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, buf.m.offset);
		if (ptr == MAP_FAILED) {
			printf("  mmap buf %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}
		munmap(ptr, buf.length);

		ret = ioctl(fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			printf("  VIDIOC_QBUF %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}
	}

	/* Try to start streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("  VIDIOC_STREAMON FAILED: %s (errno=%d)\n",
		       strerror(errno), errno);
		if (errno == EPIPE)
			printf("  -> Pipeline validation error (format mismatch)\n");

		/* Cleanup buffers */
		memset(&req, 0, sizeof(req));
		req.count  = 0;
		req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;
		ioctl(fd, VIDIOC_REQBUFS, &req);
		return -1;
	}

	printf("  VIDIOC_STREAMON SUCCESS\n");

	/* Stop streaming immediately */
	ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
		printf("  VIDIOC_STREAMOFF failed: %s\n", strerror(errno));

cleanup:
	/* Free buffers */
	memset(&req, 0, sizeof(req));
	req.count  = 0;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(fd, VIDIOC_REQBUFS, &req);

	return (ret < 0) ? -1 : 0;
}

int main(int argc, char *argv[])
{
	const char *video_dev  = (argc > 1) ? argv[1] : DEFAULT_VIDEO_DEV;
	const char *sensor_dev = (argc > 2) ? argv[2] : DEFAULT_SENSOR_DEV;
	const char *csi2_dev   = (argc > 3) ? argv[3] : DEFAULT_CSI2_DEV;
	int video_fd, sensor_fd, csi2_fd;
	int test1_result, test2_result;

	printf("=== VSOC-3000 Pipeline Validation Test (Part 10) ===\n\n");
	printf("Devices: video=%s sensor=%s csi2=%s\n\n",
	       video_dev, sensor_dev, csi2_dev);

	/* Open devices */
	video_fd = open(video_dev, O_RDWR);
	if (video_fd < 0) {
		if (errno == ENOENT || errno == ENODEV) {
			printf("ERROR: %s not found.\n", video_dev);
			printf("Make sure the modules are loaded:\n");
			printf("  sudo insmod soc_hw_platform.ko\n");
			printf("  sudo insmod vsoc_csi2.ko\n");
			printf("  sudo insmod vsoc_bridge.ko\n");
			printf("  sudo insmod vsoc_sensor.ko\n");
		} else {
			perror("open video");
		}
		return 1;
	}

	sensor_fd = open(sensor_dev, O_RDWR);
	if (sensor_fd < 0) {
		printf("WARNING: cannot open %s: %s\n",
		       sensor_dev, strerror(errno));
		printf("Subdev device nodes may not be available.\n");
		printf("Continuing with video device only...\n\n");
		sensor_fd = -1;
	}

	csi2_fd = open(csi2_dev, O_RDWR);
	if (csi2_fd < 0) {
		printf("WARNING: cannot open %s: %s\n",
		       csi2_dev, strerror(errno));
		printf("CSI-2 subdev device node may not be available.\n");
		printf("Continuing without CSI-2 format control...\n\n");
		csi2_fd = -1;
	}

	/* ============================================================
	 * Test 1: Matching formats — streaming should succeed
	 * ============================================================ */
	printf("========================================\n");
	printf("TEST 1: Matching formats (1920x1080)\n");
	printf("========================================\n\n");

	/* Set 1920x1080 on all entities */
	/* SRGGB10 = 0x300f in media bus format enum */
	if (sensor_fd >= 0)
		set_subdev_fmt(sensor_fd, "sensor", 0, 1920, 1080, 0x300f);
	if (csi2_fd >= 0) {
		set_subdev_fmt(csi2_fd, "csi2", 0, 1920, 1080, 0x300f);
		set_subdev_fmt(csi2_fd, "csi2", 1, 1920, 1080, 0x300f);
	}
	set_video_fmt(video_fd, 1920, 1080);

	test1_result = try_stream(video_fd, "Test 1");

	/* ============================================================
	 * Test 2: Mismatched formats — streaming should fail
	 * ============================================================ */
	printf("\n========================================\n");
	printf("TEST 2: Mismatched formats\n");
	printf("  sensor=1280x720, csi2=1920x1080\n");
	printf("========================================\n\n");

	/* Set different resolution on sensor only */
	if (sensor_fd >= 0)
		set_subdev_fmt(sensor_fd, "sensor", 0, 1280, 720, 0x300f);
	/* CSI-2 still at 1920x1080 (mismatch!) */
	set_video_fmt(video_fd, 1920, 1080);

	test2_result = try_stream(video_fd, "Test 2");

	/* ============================================================
	 * Results Summary
	 * ============================================================ */
	printf("\n========================================\n");
	printf("RESULTS SUMMARY\n");
	printf("========================================\n\n");

	printf("  Test 1 (matching formats):    %s %s\n",
	       (test1_result == 0) ? "PASS" : "FAIL",
	       (test1_result == 0) ? "(streaming succeeded)" :
				      "(streaming should have succeeded)");

	printf("  Test 2 (mismatched formats):  %s %s\n",
	       (test2_result != 0) ? "PASS" : "FAIL",
	       (test2_result != 0) ? "(streaming correctly refused)" :
				      "(streaming should have been refused)");

	printf("\n");

	if (test1_result == 0 && test2_result != 0)
		printf("All pipeline validation tests PASSED.\n");
	else
		printf("Some tests FAILED. Check kernel log (dmesg) "
		       "for details.\n");

	/* Cleanup */
	if (csi2_fd >= 0)
		close(csi2_fd);
	if (sensor_fd >= 0)
		close(sensor_fd);
	close(video_fd);

	return (test1_result == 0 && test2_result != 0) ? 0 : 1;
}
