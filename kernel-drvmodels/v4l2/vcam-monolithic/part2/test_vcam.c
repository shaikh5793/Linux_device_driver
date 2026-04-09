/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * test_vcam.c - Userspace test for vcam virtual capture device
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static int find_vcam_device(void)
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
		    strcmp((char *)cap.driver, "vcam") == 0) {
			printf("Found vcam at %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(void)
{
	struct v4l2_capability cap;
	struct v4l2_fmtdesc fmtdesc;
	struct v4l2_format fmt;
	struct v4l2_frmsizeenum fsize;
	int fd, pass = 1;

	printf("=== vcam Test ===\n\n");

	fd = find_vcam_device();
	if (fd < 0) {
		printf("FAIL: vcam device not found\n");
		return 1;
	}

	/* QUERYCAP */
	printf("[1] VIDIOC_QUERYCAP\n");
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("  QUERYCAP");
		pass = 0;
	} else {
		printf("  Driver:   %s\n", cap.driver);
		printf("  Card:     %s\n", cap.card);
		printf("  Bus:      %s\n", cap.bus_info);
		printf("  Caps:     0x%08x\n", cap.device_caps);
		if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
			printf("  FAIL: VIDEO_CAPTURE not set\n");
			pass = 0;
		} else {
			printf("  OK: VIDEO_CAPTURE supported\n");
		}
	}
	printf("\n");

	/* ENUM_FMT */
	printf("[2] VIDIOC_ENUM_FMT\n");
	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmtdesc.index = 0;
	if (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) {
		perror("  ENUM_FMT");
		pass = 0;
	} else {
		printf("  Format 0: %s (0x%08x)\n",
		       fmtdesc.description, fmtdesc.pixelformat);
		if (fmtdesc.pixelformat != V4L2_PIX_FMT_RGB24) {
			printf("  FAIL: expected RGB24\n");
			pass = 0;
		} else {
			printf("  OK: RGB24 format\n");
		}
	}
	printf("\n");

	/* G_FMT */
	printf("[3] VIDIOC_G_FMT\n");
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("  G_FMT");
		pass = 0;
	} else {
		printf("  Current: %ux%u, bytesperline=%u, sizeimage=%u\n",
		       fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
	}
	printf("\n");

	/* S_FMT */
	printf("[4] VIDIOC_S_FMT (320x240)\n");
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 320;
	fmt.fmt.pix.height = 240;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("  S_FMT");
		pass = 0;
	} else {
		printf("  Set to: %ux%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
		if (fmt.fmt.pix.width != 320 || fmt.fmt.pix.height != 240) {
			printf("  FAIL: expected 320x240, got %ux%u\n",
			       fmt.fmt.pix.width, fmt.fmt.pix.height);
			pass = 0;
		} else {
			printf("  OK: format accepted\n");
		}
	}
	printf("\n");

	/* ENUM_FRAMESIZES */
	printf("[5] VIDIOC_ENUM_FRAMESIZES\n");
	memset(&fsize, 0, sizeof(fsize));
	fsize.index = 0;
	fsize.pixel_format = V4L2_PIX_FMT_RGB24;
	if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) < 0) {
		perror("  ENUM_FRAMESIZES");
		pass = 0;
	} else {
		printf("  Type: stepwise\n");
		printf("  Range: %ux%u to %ux%u (step %ux%u)\n",
		       fsize.stepwise.min_width, fsize.stepwise.min_height,
		       fsize.stepwise.max_width, fsize.stepwise.max_height,
		       fsize.stepwise.step_width, fsize.stepwise.step_height);
	}
	printf("\n");

	close(fd);

	printf("=== Result: %s ===\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
