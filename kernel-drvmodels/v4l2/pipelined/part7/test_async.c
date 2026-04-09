// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Async Binding Test — Part 7
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Prints instructions for testing load-order independence with
 * the async subdev binding mechanism.
 *
 * Usage: ./test_async
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static int find_vsoc_device(char *path, size_t len)
{
	struct v4l2_capability cap;
	char devpath[64];
	int fd, i;

	for (i = 0; i < 16; i++) {
		snprintf(devpath, sizeof(devpath), "/dev/video%d", i);
		fd = open(devpath, O_RDWR);
		if (fd < 0)
			continue;

		memset(&cap, 0, sizeof(cap));
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			if (strstr((char *)cap.driver, "vsoc_bridge")) {
				snprintf(path, len, "%s", devpath);
				close(fd);
				return 0;
			}
		}
		close(fd);
	}
	return -1;
}

static int check_video_device(void)
{
	struct v4l2_capability cap;
	char devpath[64];
	int fd;

	if (find_vsoc_device(devpath, sizeof(devpath)) < 0) {
		printf("  RESULT: /dev/videoN NOT found (sensor not yet bound?)\n");
		return -1;
	}

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		printf("  RESULT: found %s but cannot open: %s\n",
		       devpath, strerror(errno));
		return -1;
	}

	memset(&cap, 0, sizeof(cap));
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		printf("  RESULT: QUERYCAP failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	printf("  RESULT: %s found!\n", devpath);
	printf("    Driver: %s\n", cap.driver);
	printf("    Card:   %s\n", cap.card);
	printf("    Caps:   0x%08x\n", cap.device_caps);

	if ((cap.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING))
	    == (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING))
		printf("    CAPTURE + STREAMING: OK\n");
	else
		printf("    WARNING: missing expected capabilities\n");

	close(fd);
	return 0;
}

int main(void)
{
	printf("=== VSOC-3000 Part 7: Async Subdev Binding Test ===\n\n");

	printf("This test verifies load-order independence.\n");
	printf("The async notifier framework allows sensor and bridge\n");
	printf("to be loaded in ANY order.\n\n");

	printf("-----------------------------------------------------\n");
	printf("TEST A: Sensor-first load order\n");
	printf("-----------------------------------------------------\n");
	printf("  sudo rmmod vsoc_bridge 2>/dev/null\n");
	printf("  sudo rmmod vsoc_sensor 2>/dev/null\n");
	printf("  sudo rmmod soc_hw_platform 2>/dev/null\n");
	printf("  sudo insmod ../hw/soc_hw_platform.ko\n");
	printf("  sudo insmod vsoc_sensor.ko    # sensor first\n");
	printf("  sudo insmod vsoc_bridge.ko    # bridge second\n");
	printf("  # Expected: /dev/video0 appears after bridge loads\n");
	printf("  dmesg | tail -20\n\n");

	printf("-----------------------------------------------------\n");
	printf("TEST B: Bridge-first load order\n");
	printf("-----------------------------------------------------\n");
	printf("  sudo rmmod vsoc_bridge 2>/dev/null\n");
	printf("  sudo rmmod vsoc_sensor 2>/dev/null\n");
	printf("  sudo rmmod soc_hw_platform 2>/dev/null\n");
	printf("  sudo insmod ../hw/soc_hw_platform.ko\n");
	printf("  sudo insmod vsoc_bridge.ko    # bridge first\n");
	printf("  sudo insmod vsoc_sensor.ko    # sensor second\n");
	printf("  # Expected: /dev/video0 appears after sensor loads\n");
	printf("  dmesg | tail -20\n\n");

	printf("-----------------------------------------------------\n");
	printf("KEY DMESG MESSAGES TO LOOK FOR:\n");
	printf("-----------------------------------------------------\n");
	printf("  bridge: \"waiting for async sensor match\"\n");
	printf("  sensor: \"async-registered (waiting for bridge)\"\n");
	printf("  bridge: \"sensor subdev 'vsoc_sensor' bound\"\n");
	printf("  bridge: \"registered as /dev/videoN (async complete)\"\n\n");

	printf("-----------------------------------------------------\n");
	printf("CHECKING CURRENT STATE:\n");
	printf("-----------------------------------------------------\n");
	if (check_video_device() == 0) {
		printf("\n  Both modules appear to be loaded and bound.\n");
		printf("  Try running: ../part5/test_capture\n");
	} else {
		printf("\n  Load both modules and run this test again.\n");
	}

	printf("\n=== Test complete ===\n");
	return 0;
}
