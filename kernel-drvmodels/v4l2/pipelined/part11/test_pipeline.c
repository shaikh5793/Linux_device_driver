// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Multi-Subdev Pipeline Test -- Part 11
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Userspace test for the 4-entity pipeline:
 *   Sensor -> CSI-2 -> ISP -> DMA (video node)
 *
 * Steps:
 *   1. Open /dev/media0, enumerate entities (should show 4)
 *   2. Print the pipeline topology
 *   3. Open /dev/video0
 *   4. S_FMT 1920x1080 RGB24
 *   5. REQBUFS 4, mmap, QBUF, STREAMON
 *   6. DQBUF 5 frames, print sequence + data
 *   7. STREAMOFF, cleanup
 *   8. Print "Full 4-entity pipeline test complete"
 *
 * Usage: ./test_pipeline [/dev/video0 [/dev/media0]]
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
#include <linux/media.h>

#define DEFAULT_VIDEO_DEV	"/dev/video0"
#define DEFAULT_MEDIA_DEV	"/dev/media0"

#define NUM_BUFFERS		4
#define NUM_FRAMES		5

/* ====================================================================
 * Media Device: Enumerate Entities and Print Topology
 * ==================================================================== */

static int enumerate_entities(int media_fd)
{
	struct media_entity_desc entity;
	int count = 0;

	printf("\n--- Media Topology ---\n");

	memset(&entity, 0, sizeof(entity));
	entity.id = MEDIA_ENT_ID_FLAG_NEXT;

	while (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
		printf("  Entity %u: '%s' (type=0x%08x, pads=%u, links=%u)\n",
		       entity.id, entity.name, entity.type,
		       entity.pads, entity.links);
		count++;

		entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
	}

	printf("  Total entities: %d\n", count);

	/* Enumerate links */
	printf("\n--- Media Links ---\n");

	memset(&entity, 0, sizeof(entity));
	entity.id = MEDIA_ENT_ID_FLAG_NEXT;

	while (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
		struct media_links_enum links;
		struct media_pad_desc *pads = NULL;
		struct media_link_desc *link_descs = NULL;
		unsigned int i;

		if (entity.pads > 0)
			pads = calloc(entity.pads, sizeof(*pads));
		if (entity.links > 0)
			link_descs = calloc(entity.links, sizeof(*link_descs));

		memset(&links, 0, sizeof(links));
		links.entity = entity.id;
		links.pads = pads;
		links.links = link_descs;

		if (ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links) == 0) {
			for (i = 0; i < entity.links; i++) {
				if (link_descs[i].source.entity == entity.id) {
					printf("  [%u]:%u -> [%u]:%u",
					       link_descs[i].source.entity,
					       link_descs[i].source.index,
					       link_descs[i].sink.entity,
					       link_descs[i].sink.index);
					if (link_descs[i].flags &
					    MEDIA_LNK_FL_ENABLED)
						printf(" [ENABLED]");
					if (link_descs[i].flags &
					    MEDIA_LNK_FL_IMMUTABLE)
						printf(" [IMMUTABLE]");
					printf("\n");
				}
			}
		}

		free(pads);
		free(link_descs);

		entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
	}

	return count;
}

/* ====================================================================
 * Main Test
 * ==================================================================== */

int main(int argc, char *argv[])
{
	const char *video_dev = (argc > 1) ? argv[1] : DEFAULT_VIDEO_DEV;
	const char *media_dev = (argc > 2) ? argv[2] : DEFAULT_MEDIA_DEV;
	int video_fd, media_fd;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	void *buffers[NUM_BUFFERS];
	unsigned int buf_lengths[NUM_BUFFERS];
	enum v4l2_buf_type type;
	int entity_count;
	unsigned int i;
	int ret;

	printf("=== VSOC-3000 Multi-Subdev Pipeline Test (Part 11) ===\n");
	printf("Devices: video=%s media=%s\n", video_dev, media_dev);

	/* ---- Step 1: Open media device and enumerate ---- */
	media_fd = open(media_dev, O_RDWR);
	if (media_fd < 0) {
		printf("ERROR: cannot open %s: %s\n", media_dev,
		       strerror(errno));
		printf("Make sure modules are loaded:\n");
		printf("  sudo insmod soc_hw_platform.ko\n");
		printf("  sudo insmod vsoc_csi2.ko\n");
		printf("  sudo insmod vsoc_isp.ko\n");
		printf("  sudo insmod vsoc_bridge.ko\n");
		printf("  sudo insmod vsoc_sensor.ko\n");
		return 1;
	}

	entity_count = enumerate_entities(media_fd);
	if (entity_count < 4) {
		printf("WARNING: expected 4 entities, found %d\n",
		       entity_count);
	}
	close(media_fd);

	/* ---- Step 2: Open video device ---- */
	video_fd = open(video_dev, O_RDWR);
	if (video_fd < 0) {
		printf("ERROR: cannot open %s: %s\n", video_dev,
		       strerror(errno));
		return 1;
	}

	/* ---- Step 3: S_FMT 1920x1080 RGB24 ---- */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = 1920;
	fmt.fmt.pix.height      = 1080;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;

	ret = ioctl(video_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("ERROR: VIDIOC_S_FMT failed: %s\n", strerror(errno));
		close(video_fd);
		return 1;
	}
	printf("\nFormat set: %ux%u, pixfmt=0x%08x, sizeimage=%u\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage);

	/* ---- Step 4: REQBUFS ---- */
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(video_fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		printf("ERROR: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		close(video_fd);
		return 1;
	}
	printf("Allocated %u buffers\n", req.count);

	/* ---- Step 5: QUERYBUF + mmap + QBUF ---- */
	for (i = 0; i < req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;

		ret = ioctl(video_fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			printf("ERROR: QUERYBUF %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}

		buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, video_fd, buf.m.offset);
		if (buffers[i] == MAP_FAILED) {
			printf("ERROR: mmap buf %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}
		buf_lengths[i] = buf.length;

		ret = ioctl(video_fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			printf("ERROR: QBUF %u failed: %s\n",
			       i, strerror(errno));
			goto cleanup;
		}
	}
	printf("All buffers mapped and queued\n");

	/* ---- Step 6: STREAMON ---- */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(video_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("ERROR: STREAMON failed: %s (errno=%d)\n",
		       strerror(errno), errno);
		goto cleanup;
	}
	printf("Streaming started\n\n");

	/* ---- Step 7: DQBUF 5 frames ---- */
	for (i = 0; i < NUM_FRAMES; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		ret = ioctl(video_fd, VIDIOC_DQBUF, &buf);
		if (ret < 0) {
			printf("ERROR: DQBUF failed: %s\n", strerror(errno));
			break;
		}

		printf("Frame %u: sequence=%u, bytesused=%u, "
		       "timestamp=%lu.%06lu",
		       i, buf.sequence, buf.bytesused,
		       (unsigned long)(buf.timestamp.tv_sec),
		       (unsigned long)(buf.timestamp.tv_usec));

		/* Print first few bytes of frame data */
		if (buffers[buf.index]) {
			unsigned char *data = buffers[buf.index];

			printf(", data=[%02x %02x %02x %02x %02x %02x]",
			       data[0], data[1], data[2],
			       data[3], data[4], data[5]);
		}
		printf("\n");

		/* Re-queue the buffer */
		ret = ioctl(video_fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			printf("ERROR: re-QBUF failed: %s\n",
			       strerror(errno));
			break;
		}
	}

	/* ---- Step 8: STREAMOFF ---- */
	printf("\nStopping stream...\n");
	ret = ioctl(video_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
		printf("WARNING: STREAMOFF failed: %s\n", strerror(errno));
	else
		printf("Stream stopped\n");

cleanup:
	/* Unmap buffers */
	for (i = 0; i < req.count; i++) {
		if (buffers[i] && buffers[i] != MAP_FAILED)
			munmap(buffers[i], buf_lengths[i]);
	}

	/* Free buffers */
	memset(&req, 0, sizeof(req));
	req.count  = 0;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(video_fd, VIDIOC_REQBUFS, &req);

	close(video_fd);

	printf("\nFull 4-entity pipeline test complete\n");
	return 0;
}
