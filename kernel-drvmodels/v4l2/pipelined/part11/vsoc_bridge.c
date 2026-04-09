// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Bridge -- Part 11: Multi-Subdev Pipeline
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPTS: 4-entity pipeline with ISP subdev
 *
 * Builds on Part 10 bridge by adding ISP subdev integration:
 *   - Find ISP subdev via vsoc_hw_get_isp_pdev() + platform_get_drvdata
 *   - Register ISP subdev with v4l2_device_register_subdev
 *   - Create 4-entity link chain:
 *       sensor:0 -> csi2:0 -> csi2:1 -> isp:0 -> isp:1 -> video:0
 *   - In start_streaming: s_stream ISP, CSI-2, then sensor
 *   - In stop_streaming: sensor off, CSI-2 off, then ISP off
 *   - DMA format uses RGB24 (ISP converts Bayer -> RGB)
 *
 * Pipeline:
 *   [sensor] ---> [csi2] ---> [isp] ---> [video_device]
 *    src:0        sink:0       sink:0      sink:0
 *                 src:1        src:1
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_csi2.ko
 *   3. insmod vsoc_isp.ko
 *   4. insmod vsoc_bridge.ko
 *   5. insmod vsoc_sensor.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-async.h>
#include <media/videobuf2-vmalloc.h>
#include <media/media-device.h>
#include <media/media-entity.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME	"vsoc_bridge"
#define BRIDGE_MAX_BUF	4

/* ====================================================================
 * Driver State
 * ==================================================================== */

struct vsoc_bridge_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head       list;
};

struct vsoc_bridge {
	struct platform_device	*pdev;
	struct v4l2_device	v4l2_dev;
	struct media_device	mdev;
	struct video_device	vdev;
	struct media_pad	vid_pad;
	struct media_pipeline	pipe;
	struct vb2_queue	queue;
	struct mutex		lock;

	/* Async notifier for sensor */
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev	*sensor_sd;

	/* CSI-2 subdev (found via platform device) */
	struct v4l2_subdev	*csi2_sd;

	/* ISP subdev (found via platform device) */
	struct v4l2_subdev	*isp_sd;

	/* Buffer management */
	struct list_head	buf_list;
	spinlock_t		buf_lock;
	unsigned int		sequence;

	/* Format */
	struct v4l2_pix_format	pix_fmt;

	/* Streaming state */
	bool			streaming;
	struct timer_list	frame_timer;
};

static inline struct vsoc_bridge_buffer *
to_bridge_buf(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct vsoc_bridge_buffer, vb);
}

/* ====================================================================
 * Format Helpers — now uses RGB24 since ISP converts Bayer->RGB
 * ==================================================================== */

static void vsoc_bridge_set_default_format(struct vsoc_bridge *bridge)
{
	struct v4l2_pix_format *pix = &bridge->pix_fmt;

	pix->width       = VSOC_SENSOR_DEF_WIDTH;
	pix->height      = VSOC_SENSOR_DEF_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_RGB24;
	pix->field       = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 3;	/* RGB24: 3 bytes/pixel */
	pix->sizeimage   = pix->bytesperline * pix->height;
	pix->colorspace  = V4L2_COLORSPACE_SRGB;
}

/* ====================================================================
 * Frame Timer (simulates DMA frame completion)
 * ==================================================================== */

static void vsoc_bridge_frame_timer(struct timer_list *t)
{
	struct vsoc_bridge *bridge = timer_container_of(bridge, t, frame_timer);
	struct vsoc_bridge_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&bridge->buf_lock, flags);

	if (!list_empty(&bridge->buf_list)) {
		buf = list_first_entry(&bridge->buf_list,
				       struct vsoc_bridge_buffer, list);
		list_del(&buf->list);
		spin_unlock_irqrestore(&bridge->buf_lock, flags);

		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = bridge->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	} else {
		spin_unlock_irqrestore(&bridge->buf_lock, flags);
	}

	if (bridge->streaming)
		mod_timer(&bridge->frame_timer,
			  jiffies + msecs_to_jiffies(VSOC_HW_FRAME_MS));
}

/* ====================================================================
 * Video Node Link Validation
 * ==================================================================== */

static int vsoc_bridge_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		container_of(link->sink->entity, struct video_device, entity);
	struct vsoc_bridge *bridge = video_get_drvdata(vdev);
	struct v4l2_subdev *source_sd;
	struct v4l2_subdev_format src_fmt;
	int ret;

	/* The source must be a v4l2_subdev */
	if (!is_media_entity_v4l2_subdev(link->source->entity))
		return -EINVAL;

	source_sd = media_entity_to_v4l2_subdev(link->source->entity);

	/* Get the source pad format */
	memset(&src_fmt, 0, sizeof(src_fmt));
	src_fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	src_fmt.pad   = link->source->index;

	ret = v4l2_subdev_call(source_sd, pad, get_fmt, NULL, &src_fmt);
	if (ret)
		return ret;

	/* Check that dimensions match the video node format */
	if (src_fmt.format.width != bridge->pix_fmt.width ||
	    src_fmt.format.height != bridge->pix_fmt.height) {
		dev_err(&bridge->pdev->dev,
			"link validate: format mismatch "
			"source=%ux%u video=%ux%u\n",
			src_fmt.format.width, src_fmt.format.height,
			bridge->pix_fmt.width, bridge->pix_fmt.height);
		return -EPIPE;
	}

	return 0;
}

static const struct media_entity_operations vsoc_bridge_entity_ops = {
	.link_validate = vsoc_bridge_link_validate,
};

/* ====================================================================
 * VB2 Queue Operations
 * ==================================================================== */

static int vsoc_bridge_queue_setup(struct vb2_queue *vq,
				   unsigned int *nbuffers,
				   unsigned int *nplanes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vq);

	if (*nplanes) {
		if (sizes[0] < bridge->pix_fmt.sizeimage)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = bridge->pix_fmt.sizeimage;

	return 0;
}

static void vsoc_bridge_buf_queue(struct vb2_buffer *vb)
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vsoc_bridge_buffer *buf = to_bridge_buf(vbuf);
	unsigned long flags;

	spin_lock_irqsave(&bridge->buf_lock, flags);
	list_add_tail(&buf->list, &bridge->buf_list);
	spin_unlock_irqrestore(&bridge->buf_lock, flags);
}

/*
 * vsoc_bridge_start_streaming — Part 11
 *
 * Step 1: Validate pipeline: media_pipeline_start
 * Step 2: Set streaming state and reset sequence
 * Step 3: [NEW] Start ISP first: s_stream(1) — furthest downstream
 * Step 4: Start CSI-2: s_stream(1)
 * Step 5: Start sensor: s_stream(1) — furthest upstream, started last
 * Step 6: Start frame timer
 * Step 7: On error: unwind s_stream calls in reverse, pipeline_stop
 *
 * Changes from Part 10:
 *   + Added ISP s_stream(1) call (Step 3)
 *   Start order: ISP -> CSI-2 -> Sensor (downstream first)
 */
static int vsoc_bridge_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vq);
	int ret;

	/* Step 2: Set streaming state and reset sequence */
	bridge->sequence = 0;

	/* Step 1: Validate pipeline — media_pipeline_start */
	ret = media_pipeline_start(&bridge->vid_pad, &bridge->pipe);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"media_pipeline_start failed: %d "
			"(format mismatch in pipeline?)\n", ret);
		goto err_return_bufs;
	}

	/* Step 3: Start ISP first — s_stream(1), furthest downstream */
	if (bridge->isp_sd) {
		ret = v4l2_subdev_call(bridge->isp_sd, video, s_stream, 1);
		if (ret && ret != -ENOIOCTLCMD)
			goto err_pipe_stop;
	}

	/* Step 4: Start CSI-2 — s_stream(1) */
	if (bridge->csi2_sd) {
		ret = v4l2_subdev_call(bridge->csi2_sd, video, s_stream, 1);
		if (ret && ret != -ENOIOCTLCMD)
			goto err_stop_isp;
	}

	/* Step 5: Start sensor — s_stream(1), furthest upstream, started last */
	ret = v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 1);
	if (ret && ret != -ENOIOCTLCMD)
		goto err_stop_csi2;

	/* Step 6: Start frame timer */
	bridge->streaming = true;
	mod_timer(&bridge->frame_timer,
		  jiffies + msecs_to_jiffies(VSOC_HW_FRAME_MS));

	return 0;

err_stop_csi2:
	if (bridge->csi2_sd)
		v4l2_subdev_call(bridge->csi2_sd, video, s_stream, 0);
err_stop_isp:
	if (bridge->isp_sd)
		v4l2_subdev_call(bridge->isp_sd, video, s_stream, 0);
err_pipe_stop:
	media_pipeline_stop(&bridge->vid_pad);
err_return_bufs:
	{
		struct vsoc_bridge_buffer *buf, *tmp;
		unsigned long flags;

		spin_lock_irqsave(&bridge->buf_lock, flags);
		list_for_each_entry_safe(buf, tmp, &bridge->buf_list, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_QUEUED);
		}
		spin_unlock_irqrestore(&bridge->buf_lock, flags);
	}
	return ret;
}

/*
 * vsoc_bridge_stop_streaming — Part 11
 *
 * Step 1: Stop streaming flag and frame timer
 * Step 2: Stop sensor FIRST: s_stream(0) — stop transmitter
 * Step 3: Stop CSI-2: s_stream(0)
 * Step 4: [NEW] Stop ISP last: s_stream(0) — stop receiver
 * Step 5: Return pending buffers
 * Step 6: Release pipeline lock: media_pipeline_stop
 *
 * Changes from Part 10:
 *   + Added ISP s_stream(0) (Step 4)
 *   Stop order: Sensor -> CSI-2 -> ISP (upstream first)
 */
static void vsoc_bridge_stop_streaming(struct vb2_queue *vq)
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vq);
	struct vsoc_bridge_buffer *buf, *tmp;
	unsigned long flags;

	/* Step 1: Stop streaming flag and frame timer */
	bridge->streaming = false;
	timer_delete_sync(&bridge->frame_timer);

	/* Step 2: Stop sensor FIRST — s_stream(0), stop transmitter */
	v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 0);
	/* Step 3: Stop CSI-2 — s_stream(0) */
	if (bridge->csi2_sd)
		v4l2_subdev_call(bridge->csi2_sd, video, s_stream, 0);
	/* Step 4: Stop ISP last — s_stream(0), stop receiver */
	if (bridge->isp_sd)
		v4l2_subdev_call(bridge->isp_sd, video, s_stream, 0);

	/* Step 6: Release pipeline lock — media_pipeline_stop */
	media_pipeline_stop(&bridge->vid_pad);

	/* Step 5: Return pending buffers */
	spin_lock_irqsave(&bridge->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &bridge->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&bridge->buf_lock, flags);
}

static const struct vb2_ops vsoc_bridge_vb2_ops = {
	.queue_setup     = vsoc_bridge_queue_setup,
	.buf_queue       = vsoc_bridge_buf_queue,
	.start_streaming = vsoc_bridge_start_streaming,
	.stop_streaming  = vsoc_bridge_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ====================================================================
 * V4L2 ioctl ops — now uses RGB24 format
 * ==================================================================== */

static int vsoc_bridge_querycap(struct file *file, void *fh,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VSOC-3000 Camera", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 DRV_NAME);
	return 0;
}

static int vsoc_bridge_enum_fmt(struct file *file, void *fh,
				struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_RGB24;
	return 0;
}

static int vsoc_bridge_g_fmt(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct vsoc_bridge *bridge = video_drvdata(file);

	f->fmt.pix = bridge->pix_fmt;
	return 0;
}

static int vsoc_bridge_s_fmt(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct vsoc_bridge *bridge = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->pixelformat  = V4L2_PIX_FMT_RGB24;
	pix->width  = clamp_t(u32, pix->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	pix->height = clamp_t(u32, pix->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	pix->width  = rounddown(pix->width, VSOC_SENSOR_STEP);
	pix->height = rounddown(pix->height, VSOC_SENSOR_STEP);
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 3;
	pix->sizeimage    = pix->bytesperline * pix->height;
	pix->colorspace   = V4L2_COLORSPACE_SRGB;

	bridge->pix_fmt = *pix;
	return 0;
}

static int vsoc_bridge_try_fmt(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->pixelformat  = V4L2_PIX_FMT_RGB24;
	pix->width  = clamp_t(u32, pix->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	pix->height = clamp_t(u32, pix->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	pix->width  = rounddown(pix->width, VSOC_SENSOR_STEP);
	pix->height = rounddown(pix->height, VSOC_SENSOR_STEP);
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 3;
	pix->sizeimage    = pix->bytesperline * pix->height;
	pix->colorspace   = V4L2_COLORSPACE_SRGB;

	return 0;
}

static int vsoc_bridge_enum_input(struct file *file, void *fh,
				  struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;

	strscpy(inp->name, "Camera", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int vsoc_bridge_g_input(struct file *file, void *fh,
			       unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vsoc_bridge_s_input(struct file *file, void *fh,
			       unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

static const struct v4l2_ioctl_ops vsoc_bridge_ioctl_ops = {
	.vidioc_querycap         = vsoc_bridge_querycap,
	.vidioc_enum_fmt_vid_cap = vsoc_bridge_enum_fmt,
	.vidioc_g_fmt_vid_cap    = vsoc_bridge_g_fmt,
	.vidioc_s_fmt_vid_cap    = vsoc_bridge_s_fmt,
	.vidioc_try_fmt_vid_cap  = vsoc_bridge_try_fmt,
	.vidioc_enum_input       = vsoc_bridge_enum_input,
	.vidioc_g_input          = vsoc_bridge_g_input,
	.vidioc_s_input          = vsoc_bridge_s_input,

	.vidioc_reqbufs          = vb2_ioctl_reqbufs,
	.vidioc_querybuf         = vb2_ioctl_querybuf,
	.vidioc_qbuf             = vb2_ioctl_qbuf,
	.vidioc_dqbuf            = vb2_ioctl_dqbuf,
	.vidioc_streamon         = vb2_ioctl_streamon,
	.vidioc_streamoff        = vb2_ioctl_streamoff,

	.vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ====================================================================
 * V4L2 file operations
 * ==================================================================== */

static const struct v4l2_file_operations vsoc_bridge_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ====================================================================
 * CSI-2 and ISP Subdev Lookup Helpers
 * ==================================================================== */

struct vsoc_csi2 {
	struct platform_device	*pdev;
	struct v4l2_subdev	sd;
	/* remaining fields are opaque to the bridge */
};

struct vsoc_isp {
	struct v4l2_subdev	sd;
	/* remaining fields are opaque to the bridge */
};

static struct v4l2_subdev *vsoc_bridge_find_csi2(void)
{
	struct platform_device *csi2_pdev;
	struct vsoc_csi2 *csi2;

	csi2_pdev = vsoc_hw_get_csi2_pdev();
	if (!csi2_pdev)
		return NULL;

	csi2 = platform_get_drvdata(csi2_pdev);
	if (!csi2)
		return NULL;

	return &csi2->sd;
}

static struct v4l2_subdev *vsoc_bridge_find_isp(void)
{
	struct platform_device *isp_pdev;
	struct vsoc_isp *isp;

	isp_pdev = vsoc_hw_get_isp_pdev();
	if (!isp_pdev)
		return NULL;

	isp = platform_get_drvdata(isp_pdev);
	if (!isp)
		return NULL;

	return &isp->sd;
}

/* ====================================================================
 * Async Notifier Callbacks
 * ==================================================================== */

static int vsoc_bridge_notify_bound(struct v4l2_async_notifier *notifier,
				    struct v4l2_subdev *sd,
				    struct v4l2_async_connection *asc)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);

	bridge->sensor_sd = sd;
	dev_info(&bridge->pdev->dev, "sensor subdev '%s' bound\n", sd->name);
	return 0;
}

/*
 * vsoc_bridge_notify_complete — Part 11: Multi-Subdev Pipeline
 *
 * Step 1: Find and register CSI-2 subdev
 * Step 2: [NEW] Find and register ISP subdev
 * Step 3: Create subdev device nodes
 * Step 4: Initialize video entity sink pad with link_validate
 * Step 5: Register video_device
 * Step 6: [CHANGED] Create 3 media links for 4-entity pipeline:
 *         sensor:0 -> csi2:0, csi2:1 -> isp:0, isp:1 -> video:0
 * Step 7: Register media_device
 *
 * Changes from Part 10:
 *   + Added ISP subdev discovery and registration (Step 2)
 *   ~ Link chain extended: added csi2->isp and isp->video links (Step 6)
 *   ~ Video format changed from SRGGB10 to RGB24 (ISP converts Bayer to RGB)
 *   Pipeline: [sensor] -> [csi2] -> [isp] -> [video]
 */
static int vsoc_bridge_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);
	int ret;

	/* Step 1: Find and register CSI-2 subdev */
	bridge->csi2_sd = vsoc_bridge_find_csi2();
	if (bridge->csi2_sd) {
		ret = v4l2_device_register_subdev(&bridge->v4l2_dev,
						  bridge->csi2_sd);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"register CSI-2 subdev failed: %d\n", ret);
			return ret;
		}
		dev_info(&bridge->pdev->dev, "CSI-2 subdev registered\n");
	} else {
		dev_warn(&bridge->pdev->dev,
			 "CSI-2 subdev not found\n");
	}

	/* Step 2: Find and register ISP subdev */
	bridge->isp_sd = vsoc_bridge_find_isp();
	if (bridge->isp_sd) {
		ret = v4l2_device_register_subdev(&bridge->v4l2_dev,
						  bridge->isp_sd);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"register ISP subdev failed: %d\n", ret);
			return ret;
		}
		dev_info(&bridge->pdev->dev, "ISP subdev registered\n");
	} else {
		dev_warn(&bridge->pdev->dev,
			 "ISP subdev not found\n");
	}

	/* Step 3: Create subdev device nodes */
	/* Step 4: Initialize video entity sink pad with link_validate */
	bridge->vid_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&bridge->vdev.entity, 1,
				     &bridge->vid_pad);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"video entity pads_init failed: %d\n", ret);
		return ret;
	}
	bridge->vdev.entity.function = MEDIA_ENT_F_IO_V4L;
	bridge->vdev.entity.ops = &vsoc_bridge_entity_ops;

	/* Step 5: Register video_device */
	ret = video_register_device(&bridge->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"video_register_device failed: %d\n", ret);
		return ret;
	}

	/* Step 6: Create 4-entity media links:
	 *   sensor:0 -> csi2:0, csi2:1 -> isp:0, isp:1 -> video:0
	 */
	if (bridge->csi2_sd && bridge->isp_sd) {
		/* sensor:0 -> csi2:0 */
		ret = media_create_pad_link(
			&bridge->sensor_sd->entity, 0,
			&bridge->csi2_sd->entity, 0,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"sensor->csi2 link failed: %d\n", ret);
			goto err_unreg_video;
		}

		/* csi2:1 -> isp:0 */
		ret = media_create_pad_link(
			&bridge->csi2_sd->entity, 1,
			&bridge->isp_sd->entity, 0,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"csi2->isp link failed: %d\n", ret);
			goto err_unreg_video;
		}

		/* isp:1 -> video:0 */
		ret = media_create_pad_link(
			&bridge->isp_sd->entity, 1,
			&bridge->vdev.entity, 0,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"isp->video link failed: %d\n", ret);
			goto err_unreg_video;
		}
	} else {
		/* Fallback: direct sensor -> video */
		ret = media_create_pad_link(
			&bridge->sensor_sd->entity, 0,
			&bridge->vdev.entity, 0,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(&bridge->pdev->dev,
				"sensor->video link failed: %d\n", ret);
			goto err_unreg_video;
		}
	}

	/* Step 7: Register media_device */
	ret = media_device_register(&bridge->mdev);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"media_device_register failed: %d\n", ret);
		goto err_unreg_video;
	}

	dev_info(&bridge->pdev->dev,
		 "4-entity pipeline complete: /dev/video%d + /dev/media%d\n"
		 "  sensor -> csi2 -> isp -> video (RGB24 output)\n",
		 bridge->vdev.num, bridge->mdev.devnode->minor);
	return 0;

err_unreg_video:
	video_unregister_device(&bridge->vdev);
	return ret;
}

static const struct v4l2_async_notifier_operations vsoc_bridge_notify_ops = {
	.bound    = vsoc_bridge_notify_bound,
	.complete = vsoc_bridge_notify_complete,
};

/* ====================================================================
 * Platform Driver Probe / Remove
 * ==================================================================== */

static int vsoc_bridge_probe(struct platform_device *pdev)
{
	struct vsoc_bridge *bridge;
	struct i2c_adapter *adap;
	struct vb2_queue *vq;
	int ret;

	bridge = devm_kzalloc(&pdev->dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	bridge->pdev = pdev;
	platform_set_drvdata(pdev, bridge);
	mutex_init(&bridge->lock);
	spin_lock_init(&bridge->buf_lock);
	INIT_LIST_HEAD(&bridge->buf_list);
	timer_setup(&bridge->frame_timer, vsoc_bridge_frame_timer, 0);

	/* Set default pixel format (RGB24) */
	vsoc_bridge_set_default_format(bridge);

	/* Initialize media device */
	strscpy(bridge->mdev.model, "VSOC-3000 Camera",
		sizeof(bridge->mdev.model));
	snprintf(bridge->mdev.bus_info, sizeof(bridge->mdev.bus_info),
		 "platform:%s", dev_name(&pdev->dev));
	bridge->mdev.dev = &pdev->dev;
	bridge->mdev.hw_revision = 0x0100;
	media_device_init(&bridge->mdev);

	/* Register v4l2_device with media device */
	bridge->v4l2_dev.mdev = &bridge->mdev;
	ret = v4l2_device_register(&pdev->dev, &bridge->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "v4l2_device_register failed: %d\n", ret);
		goto err_media_cleanup;
	}

	/* Initialize VB2 queue */
	vq = &bridge->queue;
	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_READ;
	vq->drv_priv = bridge;
	vq->buf_struct_size = sizeof(struct vsoc_bridge_buffer);
	vq->ops = &vsoc_bridge_vb2_ops;
	vq->mem_ops = &vb2_vmalloc_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->lock = &bridge->lock;

	ret = vb2_queue_init(vq);
	if (ret) {
		dev_err(&pdev->dev, "vb2_queue_init failed: %d\n", ret);
		goto err_v4l2;
	}

	/* Initialize video device (registered later in .complete) */
	bridge->vdev.fops      = &vsoc_bridge_fops;
	bridge->vdev.ioctl_ops = &vsoc_bridge_ioctl_ops;
	bridge->vdev.release   = video_device_release_empty;
	bridge->vdev.v4l2_dev  = &bridge->v4l2_dev;
	bridge->vdev.queue     = &bridge->queue;
	bridge->vdev.lock      = &bridge->lock;
	bridge->vdev.vfl_dir   = VFL_DIR_RX;
	bridge->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				   V4L2_CAP_STREAMING |
				   V4L2_CAP_READWRITE;
	strscpy(bridge->vdev.name, "VSOC-3000 Capture",
		sizeof(bridge->vdev.name));
	video_set_drvdata(&bridge->vdev, bridge);

	/* Setup async notifier for sensor */
	adap = vsoc_hw_get_i2c_adapter();
	if (!adap) {
		dev_err(&pdev->dev, "I2C adapter not available\n");
		ret = -ENODEV;
		goto err_v4l2;
	}

	v4l2_async_nf_init(&bridge->notifier, &bridge->v4l2_dev);
	bridge->notifier.ops = &vsoc_bridge_notify_ops;

	{
		struct v4l2_async_connection *asc;

		asc = v4l2_async_nf_add_i2c(&bridge->notifier,
					     i2c_adapter_id(adap),
					     VSOC_SENSOR_I2C_ADDR,
					     struct v4l2_async_connection);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			dev_err(&pdev->dev,
				"v4l2_async_nf_add_i2c failed: %d\n", ret);
			goto err_v4l2;
		}
	}

	ret = v4l2_async_nf_register(&bridge->notifier);
	if (ret) {
		dev_err(&pdev->dev,
			"v4l2_async_nf_register failed: %d\n", ret);
		goto err_nf_cleanup;
	}

	dev_info(&pdev->dev,
		 "vsoc_bridge probed (4-entity pipeline mode)\n");
	return 0;

err_nf_cleanup:
	v4l2_async_nf_cleanup(&bridge->notifier);
err_v4l2:
	v4l2_device_unregister(&bridge->v4l2_dev);
err_media_cleanup:
	media_device_cleanup(&bridge->mdev);
	return ret;
}

static void vsoc_bridge_remove(struct platform_device *pdev)
{
	struct vsoc_bridge *bridge = platform_get_drvdata(pdev);

	timer_delete_sync(&bridge->frame_timer);
	video_unregister_device(&bridge->vdev);
	v4l2_async_nf_unregister(&bridge->notifier);
	v4l2_async_nf_cleanup(&bridge->notifier);
	v4l2_device_unregister(&bridge->v4l2_dev);
	media_device_unregister(&bridge->mdev);
	media_device_cleanup(&bridge->mdev);

	dev_info(&pdev->dev, "vsoc_bridge removed\n");
}

/* ====================================================================
 * Platform Driver Registration
 * ==================================================================== */

static struct platform_driver vsoc_bridge_driver = {
	.probe  = vsoc_bridge_probe,
	.remove = vsoc_bridge_remove,
	.driver = {
		.name = VSOC_BRIDGE_DEV_NAME,
	},
};

module_platform_driver(vsoc_bridge_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 Camera Bridge -- Part 11: Multi-Subdev Pipeline");
MODULE_VERSION("1.0.0");
