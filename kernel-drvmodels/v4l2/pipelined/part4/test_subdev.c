// SPDX-License-Identifier: GPL-2.0
/*
 * Test program for Part 4: Pad Format Negotiation
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Tests v4l2_subdev_pad_ops:
 *   - VIDIOC_SUBDEV_ENUM_MBUS_CODE
 *   - VIDIOC_SUBDEV_G_FMT
 *   - VIDIOC_SUBDEV_S_FMT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <linux/media-bus-format.h>

#define MAX_SUBDEV	16

static const char *mbus_code_name(unsigned int code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		return "SRGGB10_1X10 (Bayer RAW 10-bit)";
	case MEDIA_BUS_FMT_RGB888_1X24:
		return "RGB888_1X24 (24-bit RGB)";
	default:
		return "unknown";
	}
}

static int find_and_open_subdev(void)
{
	char path[64];
	int fd, i;
	struct v4l2_subdev_mbus_code_enum code;

	for (i = 0; i < MAX_SUBDEV; i++) {
		snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		/* Check if this subdev supports enum_mbus_code (our sensor) */
		memset(&code, 0, sizeof(code));
		code.pad = 0;
		code.index = 0;
		code.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		if (ioctl(fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &code) == 0) {
			printf("Found vsoc_sensor at %s\n", path);
			return fd;
		}
		close(fd);
	}

	return -1;
}

static int test_enum_mbus_codes(int fd)
{
	struct v4l2_subdev_mbus_code_enum code;
	int i;

	printf("\n=== Enumerating Media Bus Codes ===\n");

	for (i = 0; ; i++) {
		memset(&code, 0, sizeof(code));
		code.pad = 0;
		code.index = i;
		code.which = V4L2_SUBDEV_FORMAT_ACTIVE;

		if (ioctl(fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &code) < 0) {
			if (errno == EINVAL)
				break;
			perror("VIDIOC_SUBDEV_ENUM_MBUS_CODE");
			return -1;
		}

		printf("  [%d] code=0x%04x  %s\n",
		       i, code.code, mbus_code_name(code.code));
	}

	printf("  Total: %d formats\n", i);
	return 0;
}

static int test_get_fmt(int fd)
{
	struct v4l2_subdev_format fmt;

	printf("\n=== Get Current Format (pad 0) ===\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.pad = 0;
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) < 0) {
		perror("VIDIOC_SUBDEV_G_FMT");
		return -1;
	}

	printf("  Width:      %u\n", fmt.format.width);
	printf("  Height:     %u\n", fmt.format.height);
	printf("  Code:       0x%04x  %s\n",
	       fmt.format.code, mbus_code_name(fmt.format.code));
	printf("  Field:      %u\n", fmt.format.field);
	printf("  Colorspace: %u\n", fmt.format.colorspace);

	return 0;
}

static int test_set_fmt(int fd)
{
	struct v4l2_subdev_format fmt;

	printf("\n=== Set Format: 1280x720 SRGGB10 ===\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.pad = 0;
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.format.width = 1280;
	fmt.format.height = 720;
	fmt.format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt.format.field = V4L2_FIELD_NONE;
	fmt.format.colorspace = V4L2_COLORSPACE_RAW;

	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0) {
		perror("VIDIOC_SUBDEV_S_FMT");
		return -1;
	}

	printf("  Result: %ux%u code=0x%04x  %s\n",
	       fmt.format.width, fmt.format.height,
	       fmt.format.code, mbus_code_name(fmt.format.code));

	/* Read back to verify */
	printf("\n=== Verify: Get Format After Set ===\n");

	memset(&fmt, 0, sizeof(fmt));
	fmt.pad = 0;
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) < 0) {
		perror("VIDIOC_SUBDEV_G_FMT (verify)");
		return -1;
	}

	printf("  Width:  %u (expected 1280)\n", fmt.format.width);
	printf("  Height: %u (expected 720)\n", fmt.format.height);
	printf("  Code:   0x%04x  %s\n",
	       fmt.format.code, mbus_code_name(fmt.format.code));

	if (fmt.format.width == 1280 && fmt.format.height == 720 &&
	    fmt.format.code == MEDIA_BUS_FMT_SRGGB10_1X10)
		printf("  PASS: format matches\n");
	else
		printf("  FAIL: format mismatch!\n");

	return 0;
}

int main(void)
{
	int fd;
	int ret = 0;

	printf("=== VSOC-3000 Part 4: Pad Format Negotiation Test ===\n");

	fd = find_and_open_subdev();
	if (fd < 0) {
		fprintf(stderr, "Could not find vsoc_sensor subdev\n");
		return 1;
	}

	if (test_enum_mbus_codes(fd) < 0)
		ret = 1;

	if (test_get_fmt(fd) < 0)
		ret = 1;

	if (test_set_fmt(fd) < 0)
		ret = 1;

	close(fd);
	printf("\n=== Test %s ===\n", ret ? "FAILED" : "PASSED");
	return ret;
}
