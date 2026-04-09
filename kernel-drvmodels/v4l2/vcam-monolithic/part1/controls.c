/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * controls.c - V4L2 control interface
 *
 * Demonstrates the V4L2 control interface:
 *   - VIDIOC_QUERYCTRL  -- enumerate available controls and their ranges
 *   - VIDIOC_G_CTRL     -- read current control value
 *   - VIDIOC_S_CTRL     -- write a new control value
 *   - VIDIOC_QUERYMENU  -- enumerate menu items for menu-type controls
 *
 * Usage: ./controls /dev/video0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/videodev2.h>

/* Translate a V4L2 control type enum to a human-readable string. */
static const char *ctrl_type_name(int type)
{
	switch (type) {
	case V4L2_CTRL_TYPE_INTEGER:      return "Integer";
	case V4L2_CTRL_TYPE_BOOLEAN:      return "Boolean";
	case V4L2_CTRL_TYPE_MENU:         return "Menu";
	case V4L2_CTRL_TYPE_BUTTON:       return "Button";
	case V4L2_CTRL_TYPE_INTEGER64:    return "Integer64";
	case V4L2_CTRL_TYPE_STRING:       return "String";
	case V4L2_CTRL_TYPE_BITMASK:      return "Bitmask";
	case V4L2_CTRL_TYPE_INTEGER_MENU: return "IntegerMenu";
	default: return "Unknown";
	}
}

/* Print all named entries for a menu-type control using VIDIOC_QUERYMENU. */
static void print_menu_items(int fd, unsigned int id)
{
	struct v4l2_querymenu menu;

	memset(&menu, 0, sizeof(menu));
	menu.id = id;

	for (menu.index = 0; ; menu.index++) {
		if (ioctl(fd, VIDIOC_QUERYMENU, &menu) < 0)
			break;
		printf("      [%d] %s\n", menu.index, menu.name);
	}
}

/*
 * Enumerate all standard user-class controls (V4L2_CID_BASE to
 * V4L2_CID_LASTP1), printing their properties and current values.
 */
static void enumerate_controls(int fd)
{
	struct v4l2_queryctrl qctrl;
	struct v4l2_control ctrl;
	int count = 0;

	printf("[1] Available Controls (VIDIOC_QUERYCTRL)\n");
	printf("------------------------------------------\n");

	for (unsigned int id = V4L2_CID_BASE; id < V4L2_CID_LASTP1; id++) {
		memset(&qctrl, 0, sizeof(qctrl));
		qctrl.id = id;

		if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) < 0)
			continue;

		if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		count++;
		printf("  %s [%s]\n", qctrl.name, ctrl_type_name(qctrl.type));
		printf("    Range: %d..%d (step %d, default %d)\n",
		       qctrl.minimum, qctrl.maximum,
		       qctrl.step, qctrl.default_value);

		if (qctrl.flags & ~V4L2_CTRL_FLAG_DISABLED) {
			printf("    Flags:");
			if (qctrl.flags & V4L2_CTRL_FLAG_GRABBED)
				printf(" GRABBED");
			if (qctrl.flags & V4L2_CTRL_FLAG_READ_ONLY)
				printf(" READ_ONLY");
			if (qctrl.flags & V4L2_CTRL_FLAG_VOLATILE)
				printf(" VOLATILE");
			if (qctrl.flags & V4L2_CTRL_FLAG_INACTIVE)
				printf(" INACTIVE");
			printf("\n");
		}

		/* Read current value (skip buttons and write-only controls). */
		if (qctrl.type != V4L2_CTRL_TYPE_BUTTON &&
		    !(qctrl.flags & V4L2_CTRL_FLAG_WRITE_ONLY)) {
			memset(&ctrl, 0, sizeof(ctrl));
			ctrl.id = id;
			if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0)
				printf("    Current: %d\n", ctrl.value);
		}

		if (qctrl.type == V4L2_CTRL_TYPE_MENU)
			print_menu_items(fd, id);

		printf("\n");
	}

	if (count == 0)
		printf("  No standard controls found.\n\n");
}

/*
 * Demonstrate the read-modify-verify pattern for V4L2_CID_BRIGHTNESS:
 * read the current value, set a new one, verify the readback, then restore.
 */
static void control_modification_demo(int fd)
{
	struct v4l2_queryctrl qctrl;
	struct v4l2_control ctrl;
	int original, target;

	printf("[2] Control Modification Demo (S_CTRL / G_CTRL)\n");
	printf("-------------------------------------------------\n");

	memset(&qctrl, 0, sizeof(qctrl));
	qctrl.id = V4L2_CID_BRIGHTNESS;

	if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) < 0 ||
	    (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)) {
		printf("  Brightness not available -- skipping demo\n\n");
		return;
	}

	/* Step 1: Read current value. */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = V4L2_CID_BRIGHTNESS;
	if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
		printf("  Cannot read brightness: %s\n\n", strerror(errno));
		return;
	}
	original = ctrl.value;
	printf("  Current brightness: %d\n", original);

	/* Step 2: Set brightness to the midpoint of its valid range. */
	target = (qctrl.minimum + qctrl.maximum) / 2;
	ctrl.value = target;
	if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		printf("  Cannot set brightness: %s\n\n", strerror(errno));
		return;
	}
	printf("  Set brightness to: %d\n", target);

	/* Step 3: Verify -- the driver may have clamped or rounded. */
	ctrl.value = 0;
	if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0)
		printf("  Readback confirms: %d\n", ctrl.value);

	/* Restore the original value. */
	ctrl.value = original;
	ioctl(fd, VIDIOC_S_CTRL, &ctrl);
	printf("  Restored to: %d\n\n", original);
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
	printf("V4L2 Control Interface -- %s\n", device_path);
	printf("================================\n\n");

	fd = open(device_path, O_RDWR);
	if (fd < 0) {
		printf("Cannot open %s: %s\n", device_path, strerror(errno));
		return 1;
	}

	enumerate_controls(fd);
	control_modification_demo(fd);

	close(fd);
	return 0;
}
