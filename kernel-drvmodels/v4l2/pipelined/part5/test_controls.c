// SPDX-License-Identifier: GPL-2.0
/*
 * Test program for Part 5: Subdev Controls
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Tests v4l2_ctrl_handler:
 *   - VIDIOC_QUERYCTRL to enumerate controls
 *   - VIDIOC_G_CTRL / VIDIOC_S_CTRL to get/set values
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

#define MAX_SUBDEV	16

static const char *ctrl_name(unsigned int id)
{
	switch (id) {
	case V4L2_CID_EXPOSURE:		return "Exposure";
	case V4L2_CID_ANALOGUE_GAIN:	return "Analogue Gain";
	case V4L2_CID_DIGITAL_GAIN:	return "Digital Gain";
	case V4L2_CID_HFLIP:		return "Horizontal Flip";
	case V4L2_CID_VFLIP:		return "Vertical Flip";
	default:			return "Unknown";
	}
}

static int find_and_open_subdev(void)
{
	char path[64];
	int fd, i;
	struct v4l2_queryctrl qctrl;

	for (i = 0; i < MAX_SUBDEV; i++) {
		snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		/* Check if this subdev has exposure control (our sensor) */
		memset(&qctrl, 0, sizeof(qctrl));
		qctrl.id = V4L2_CID_EXPOSURE;
		if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
			printf("Found vsoc_sensor at %s\n", path);
			return fd;
		}
		close(fd);
	}

	return -1;
}

static int get_ctrl(int fd, unsigned int id)
{
	struct v4l2_control ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
		perror("VIDIOC_G_CTRL");
		return -1;
	}
	return ctrl.value;
}

static int set_ctrl(int fd, unsigned int id, int value)
{
	struct v4l2_control ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	ctrl.value = value;
	if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		perror("VIDIOC_S_CTRL");
		return -1;
	}
	return 0;
}

static int test_enumerate_controls(int fd)
{
	struct v4l2_queryctrl qctrl;
	unsigned int id;
	int count = 0;

	printf("\n=== Enumerating Controls ===\n");

	/* Iterate through user controls */
	memset(&qctrl, 0, sizeof(qctrl));
	qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;

	while (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
		if (!(qctrl.flags & V4L2_CTRL_FLAG_DISABLED)) {
			printf("  [0x%08x] %-20s  min=%-6d max=%-6d step=%-3d default=%-6d\n",
			       qctrl.id, qctrl.name,
			       qctrl.minimum, qctrl.maximum,
			       qctrl.step, qctrl.default_value);
			count++;
		}
		id = qctrl.id;
		memset(&qctrl, 0, sizeof(qctrl));
		qctrl.id = id | V4L2_CTRL_FLAG_NEXT_CTRL;
	}

	printf("  Total: %d controls\n", count);
	return count > 0 ? 0 : -1;
}

static int test_exposure(int fd)
{
	int val;

	printf("\n=== Test Exposure Control ===\n");

	val = get_ctrl(fd, V4L2_CID_EXPOSURE);
	if (val < 0)
		return -1;
	printf("  Current exposure: %d\n", val);

	printf("  Setting exposure to 5000...\n");
	if (set_ctrl(fd, V4L2_CID_EXPOSURE, 5000) < 0)
		return -1;

	val = get_ctrl(fd, V4L2_CID_EXPOSURE);
	if (val < 0)
		return -1;
	printf("  Read back exposure: %d\n", val);

	if (val == 5000)
		printf("  PASS: exposure set correctly\n");
	else
		printf("  FAIL: expected 5000, got %d\n", val);

	return (val == 5000) ? 0 : -1;
}

static int test_gain(int fd)
{
	int val;

	printf("\n=== Test Analogue Gain Control ===\n");

	val = get_ctrl(fd, V4L2_CID_ANALOGUE_GAIN);
	if (val < 0)
		return -1;
	printf("  Current gain: %d\n", val);

	printf("  Setting gain to 128...\n");
	if (set_ctrl(fd, V4L2_CID_ANALOGUE_GAIN, 128) < 0)
		return -1;

	val = get_ctrl(fd, V4L2_CID_ANALOGUE_GAIN);
	if (val < 0)
		return -1;
	printf("  Read back gain: %d\n", val);

	if (val == 128)
		printf("  PASS: gain set correctly\n");
	else
		printf("  FAIL: expected 128, got %d\n", val);

	return (val == 128) ? 0 : -1;
}

static int test_hflip(int fd)
{
	int val, new_val;

	printf("\n=== Test Horizontal Flip Control ===\n");

	val = get_ctrl(fd, V4L2_CID_HFLIP);
	if (val < 0)
		return -1;
	printf("  Current hflip: %d\n", val);

	new_val = val ? 0 : 1;
	printf("  Toggling hflip to %d...\n", new_val);
	if (set_ctrl(fd, V4L2_CID_HFLIP, new_val) < 0)
		return -1;

	val = get_ctrl(fd, V4L2_CID_HFLIP);
	if (val < 0)
		return -1;
	printf("  Read back hflip: %d\n", val);

	if (val == new_val)
		printf("  PASS: hflip toggled correctly\n");
	else
		printf("  FAIL: expected %d, got %d\n", new_val, val);

	return (val == new_val) ? 0 : -1;
}

int main(void)
{
	int fd;
	int ret = 0;

	printf("=== VSOC-3000 Part 5: Subdev Controls Test ===\n");

	fd = find_and_open_subdev();
	if (fd < 0) {
		fprintf(stderr, "Could not find vsoc_sensor subdev\n");
		return 1;
	}

	if (test_enumerate_controls(fd) < 0)
		ret = 1;

	if (test_exposure(fd) < 0)
		ret = 1;

	if (test_gain(fd) < 0)
		ret = 1;

	if (test_hflip(fd) < 0)
		ret = 1;

	close(fd);
	printf("\n=== Test %s ===\n", ret ? "FAILED" : "PASSED");
	return ret;
}
