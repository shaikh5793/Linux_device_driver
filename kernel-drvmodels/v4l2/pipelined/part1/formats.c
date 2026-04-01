/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * formats.c - V4L2 format negotiation
 *
 * Usage: ./formats /dev/video0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <linux/videodev2.h>

/* Convert a 32-bit FOURCC code to a printable 4-char string. */
static const char *fourcc_to_string(uint32_t fourcc)
{
	static char buf[5];

	buf[0] = fourcc & 0xff;
	buf[1] = (fourcc >> 8) & 0xff;
	buf[2] = (fourcc >> 16) & 0xff;
	buf[3] = (fourcc >> 24) & 0xff;
	buf[4] = '\0';
	return buf;
}

/* Enumerate supported pixel formats via VIDIOC_ENUM_FMT. */
static void enumerate_formats(int fd)
{
	struct v4l2_fmtdesc fmt;

	printf("[1] Supported Pixel Formats (VIDIOC_ENUM_FMT)\n");
	printf("----------------------------------------------\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (fmt.index = 0; ; fmt.index++) {
		if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
			if (errno == EINVAL)
				break;
			perror("  ENUM_FMT");
			break;
		}

		printf("  [%d] %s (%s)", fmt.index,
		       fmt.description, fourcc_to_string(fmt.pixelformat));
		if (fmt.flags & V4L2_FMT_FLAG_COMPRESSED)
			printf(" [COMPRESSED]");
		if (fmt.flags & V4L2_FMT_FLAG_EMULATED)
			printf(" [EMULATED]");
		printf("\n");
	}
	printf("  Total: %d format(s)\n\n", fmt.index);
}

/* Enumerate supported resolutions for a given pixel format. */
static void enumerate_frame_sizes(int fd, uint32_t pixelformat)
{
	struct v4l2_frmsizeenum frmsize;

	printf("  Frame sizes for %s:\n", fourcc_to_string(pixelformat));

	memset(&frmsize, 0, sizeof(frmsize));
	frmsize.pixel_format = pixelformat;

	for (frmsize.index = 0; ; frmsize.index++) {
		if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0)
			break;

		switch (frmsize.type) {
		case V4L2_FRMSIZE_TYPE_DISCRETE:
			printf("    %dx%d\n",
			       frmsize.discrete.width,
			       frmsize.discrete.height);
			break;
		case V4L2_FRMSIZE_TYPE_STEPWISE:
			printf("    %dx%d to %dx%d (step %dx%d)\n",
			       frmsize.stepwise.min_width,
			       frmsize.stepwise.min_height,
			       frmsize.stepwise.max_width,
			       frmsize.stepwise.max_height,
			       frmsize.stepwise.step_width,
			       frmsize.stepwise.step_height);
			break;
		case V4L2_FRMSIZE_TYPE_CONTINUOUS:
			printf("    %dx%d to %dx%d (any)\n",
			       frmsize.stepwise.min_width,
			       frmsize.stepwise.min_height,
			       frmsize.stepwise.max_width,
			       frmsize.stepwise.max_height);
			break;
		}
	}
}

/* Enumerate frame sizes for every supported pixel format. */
static void enumerate_all_frame_sizes(int fd)
{
	struct v4l2_fmtdesc fmt;

	printf("[2] Frame Sizes per Format (VIDIOC_ENUM_FRAMESIZES)\n");
	printf("----------------------------------------------------\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (fmt.index = 0; ; fmt.index++) {
		if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
			break;
		enumerate_frame_sizes(fd, fmt.pixelformat);
	}
	printf("\n");
}

/* Demonstrate G_FMT, TRY_FMT, and S_FMT format negotiation. */
static void format_negotiation_demo(int fd)
{
	struct v4l2_format fmt;

	printf("[3] Format Negotiation (G_FMT / TRY_FMT / S_FMT)\n");
	printf("--------------------------------------------------\n");

	/* G_FMT: read the device's current format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
		printf("  Current: %dx%d %s (stride=%u, size=%u)\n",
		       fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fourcc_to_string(fmt.fmt.pix.pixelformat),
		       fmt.fmt.pix.bytesperline,
		       fmt.fmt.pix.sizeimage);
	}

	/* TRY_FMT: probe 1280x720 RGB24 without changing the device */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 1280;
	fmt.fmt.pix.height = 720;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) == 0) {
		printf("  TRY 1280x720 RGB24 → driver says: %dx%d %s\n",
		       fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fourcc_to_string(fmt.fmt.pix.pixelformat));
	} else {
		printf("  TRY_FMT not supported: %s\n", strerror(errno));
	}

	/* S_FMT: apply 640x480 RGB24; always check returned values */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
		printf("  SET 640x480 RGB24 → accepted: %dx%d (size=%u)\n",
		       fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fmt.fmt.pix.sizeimage);
	} else {
		printf("  S_FMT failed: %s\n", strerror(errno));
	}
	printf("\n");
}

int main(int argc, char *argv[])
{
	int fd;
	const char *device_path;

	if (argc != 2) {
		printf("Usage: %s /dev/videoN\n", argv[0]);
		return 1;
	}

	device_path = argv[1];
	printf("V4L2 Format Negotiation — %s\n", device_path);
	printf("====================================\n\n");

	fd = open(device_path, O_RDWR);
	if (fd < 0) {
		printf("Cannot open %s: %s\n", device_path, strerror(errno));
		return 1;
	}

	enumerate_formats(fd);
	enumerate_all_frame_sizes(fd);
	format_negotiation_demo(fd);

	close(fd);
	return 0;
}
