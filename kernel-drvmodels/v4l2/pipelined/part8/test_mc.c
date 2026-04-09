// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Media Controller Test -- Part 8
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Userspace test that exercises the media controller API:
 *   - MEDIA_IOC_DEVICE_INFO: print model, driver, bus_info
 *   - MEDIA_IOC_ENUM_ENTITIES: enumerate all entities
 *
 * Usage: ./test_mc [/dev/media0]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/media.h>

#define DEFAULT_MEDIA_DEV "/dev/media0"

static const char *entity_type_name(unsigned int type)
{
	switch (type) {
	case MEDIA_ENT_F_CAM_SENSOR:		return "CAM_SENSOR";
	case MEDIA_ENT_F_VID_IF_BRIDGE:		return "VID_IF_BRIDGE";
	case MEDIA_ENT_F_IO_V4L:		return "IO_V4L";
	case MEDIA_ENT_F_PROC_VIDEO_ISP:	return "PROC_VIDEO_ISP";
	case MEDIA_ENT_F_IO_DTV:		return "IO_DTV";
	default:				return "UNKNOWN";
	}
}

int main(int argc, char *argv[])
{
	const char *dev = (argc > 1) ? argv[1] : DEFAULT_MEDIA_DEV;
	struct media_device_info info;
	struct media_entity_desc entity;
	int fd;
	int ret;
	unsigned int id;

	printf("=== VSOC-3000 Media Controller Test (Part 8) ===\n\n");

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV) {
			printf("ERROR: %s not found.\n", dev);
			printf("Make sure the modules are loaded:\n");
			printf("  sudo insmod soc_hw_platform.ko\n");
			printf("  sudo insmod vsoc_bridge.ko\n");
			printf("  sudo insmod vsoc_sensor.ko\n");
		} else {
			perror("open");
		}
		return 1;
	}

	/* Query media device info */
	memset(&info, 0, sizeof(info));
	ret = ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info);
	if (ret < 0) {
		perror("MEDIA_IOC_DEVICE_INFO");
		close(fd);
		return 1;
	}

	printf("Media Device Info:\n");
	printf("  Driver:   %s\n", info.driver);
	printf("  Model:    %s\n", info.model);
	printf("  Serial:   %s\n", info.serial);
	printf("  Bus Info: %s\n", info.bus_info);
	printf("  Version:  %u.%u.%u\n",
	       (info.media_version >> 16) & 0xff,
	       (info.media_version >> 8) & 0xff,
	       info.media_version & 0xff);
	printf("  HW Rev:   0x%08x\n", info.hw_revision);
	printf("\n");

	/* Enumerate all entities */
	printf("Entities:\n");
	printf("  %-6s %-30s %-20s %-6s\n",
	       "ID", "Name", "Type", "Pads");
	printf("  %-6s %-30s %-20s %-6s\n",
	       "------", "------------------------------",
	       "--------------------", "------");

	id = 0;
	for (;;) {
		memset(&entity, 0, sizeof(entity));
		entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;

		ret = ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity);
		if (ret < 0) {
			if (errno == EINVAL)
				break; /* No more entities */
			perror("MEDIA_IOC_ENUM_ENTITIES");
			break;
		}

		printf("  %-6u %-30s %-20s %-6u\n",
		       entity.id,
		       entity.name,
		       entity_type_name(entity.type),
		       entity.pads);

		id = entity.id;
	}

	printf("\nDone. Found entities above.\n");

	close(fd);
	return 0;
}
