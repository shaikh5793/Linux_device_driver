// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Media Topology Test -- Part 9
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Userspace test that exercises the media controller topology API:
 *   - MEDIA_IOC_ENUM_ENTITIES: enumerate all entities
 *   - MEDIA_IOC_ENUM_LINKS:   enumerate links for each entity
 *   - Print full pipeline topology in readable format
 *
 * Usage: ./test_topology [/dev/media0]
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
#define MAX_ENTITIES 16
#define MAX_PADS     8
#define MAX_LINKS    16

static const char *entity_type_name(unsigned int type)
{
	switch (type) {
	case MEDIA_ENT_F_CAM_SENSOR:		return "CAM_SENSOR";
	case MEDIA_ENT_F_VID_IF_BRIDGE:		return "VID_IF_BRIDGE";
	case MEDIA_ENT_F_IO_V4L:		return "IO_V4L";
	case MEDIA_ENT_F_PROC_VIDEO_ISP:	return "PROC_VIDEO_ISP";
	default:				return "UNKNOWN";
	}
}

static const char *pad_type_name(unsigned int flags)
{
	if (flags & MEDIA_PAD_FL_SOURCE)
		return "SOURCE";
	if (flags & MEDIA_PAD_FL_SINK)
		return "SINK";
	return "UNKNOWN";
}

static const char *link_flags_str(unsigned int flags, char *buf, size_t len)
{
	buf[0] = '\0';

	if (flags & MEDIA_LNK_FL_ENABLED)
		strncat(buf, "ENABLED", len - strlen(buf) - 1);
	if (flags & MEDIA_LNK_FL_IMMUTABLE) {
		if (buf[0])
			strncat(buf, "|", len - strlen(buf) - 1);
		strncat(buf, "IMMUTABLE", len - strlen(buf) - 1);
	}
	if (!buf[0])
		strncpy(buf, "NONE", len - 1);

	return buf;
}

struct entity_info {
	struct media_entity_desc desc;
};

int main(int argc, char *argv[])
{
	const char *dev = (argc > 1) ? argv[1] : DEFAULT_MEDIA_DEV;
	struct media_device_info info;
	struct entity_info entities[MAX_ENTITIES];
	int num_entities = 0;
	int fd;
	int ret;
	unsigned int id;
	int i;

	printf("=== VSOC-3000 Media Topology Test (Part 9) ===\n\n");

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV) {
			printf("ERROR: %s not found.\n", dev);
			printf("Make sure the modules are loaded:\n");
			printf("  sudo insmod soc_hw_platform.ko\n");
			printf("  sudo insmod vsoc_csi2.ko\n");
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

	printf("Media Device: %s\n", info.model);
	printf("  Driver:   %s\n", info.driver);
	printf("  Bus Info: %s\n", info.bus_info);
	printf("\n");

	/* Enumerate all entities */
	printf("--- Entities ---\n\n");
	id = 0;
	for (;;) {
		struct media_entity_desc *desc;

		if (num_entities >= MAX_ENTITIES) {
			printf("WARNING: too many entities, truncating\n");
			break;
		}

		desc = &entities[num_entities].desc;
		memset(desc, 0, sizeof(*desc));
		desc->id = id | MEDIA_ENT_ID_FLAG_NEXT;

		ret = ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, desc);
		if (ret < 0) {
			if (errno == EINVAL)
				break;
			perror("MEDIA_IOC_ENUM_ENTITIES");
			break;
		}

		printf("  Entity %u: \"%s\"\n", desc->id, desc->name);
		printf("    Type:  %s (0x%08x)\n",
		       entity_type_name(desc->type), desc->type);
		printf("    Pads:  %u\n", desc->pads);
		printf("    Links: %u\n", desc->links);
		printf("\n");

		id = desc->id;
		num_entities++;
	}

	printf("Total: %d entities\n\n", num_entities);

	/* Enumerate links for each entity */
	printf("--- Links ---\n\n");

	for (i = 0; i < num_entities; i++) {
		struct media_entity_desc *desc = &entities[i].desc;
		struct media_pad_desc pads[MAX_PADS];
		struct media_link_desc links[MAX_LINKS];
		struct media_links_enum lenum;
		unsigned int j;
		char flags_buf[64];

		memset(&lenum, 0, sizeof(lenum));
		memset(pads, 0, sizeof(pads));
		memset(links, 0, sizeof(links));

		lenum.entity = desc->id;
		lenum.pads = pads;
		lenum.links = links;

		ret = ioctl(fd, MEDIA_IOC_ENUM_LINKS, &lenum);
		if (ret < 0) {
			printf("  Entity %u \"%s\": MEDIA_IOC_ENUM_LINKS failed: %s\n",
			       desc->id, desc->name, strerror(errno));
			continue;
		}

		printf("  Entity %u \"%s\":\n", desc->id, desc->name);

		/* Print pads */
		for (j = 0; j < desc->pads && j < MAX_PADS; j++) {
			printf("    Pad %u: %s (flags=0x%x)\n",
			       pads[j].index,
			       pad_type_name(pads[j].flags),
			       pads[j].flags);
		}

		/* Print links */
		for (j = 0; j < desc->links && j < MAX_LINKS; j++) {
			printf("    Link: [%u:%u] -> [%u:%u] [%s]\n",
			       links[j].source.entity,
			       links[j].source.index,
			       links[j].sink.entity,
			       links[j].sink.index,
			       link_flags_str(links[j].flags,
					      flags_buf, sizeof(flags_buf)));
		}
		printf("\n");
	}

	/* Print topology summary */
	printf("--- Topology Summary ---\n\n");
	printf("  ");
	for (i = 0; i < num_entities; i++) {
		printf("[%s]", entities[i].desc.name);
		if (i < num_entities - 1)
			printf(" --> ");
	}
	printf("\n\n");

	printf("Done.\n");

	close(fd);
	return 0;
}
