// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Bridge -- Part 8: Media Controller Basics
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: media_device initialization and registration
 *
 * Builds on Part 7 by adding:
 *   - struct media_device embedded in the bridge
 *   - media_device_init() in probe, before async notifier
 *   - bridge->v4l2_dev.mdev = &bridge->mdev
 *   - media_device_register() in .complete callback
 *   - media_device_unregister/cleanup in remove
 *
 * The bridge is a platform driver on "vsoc_bridge" that:
 *   - Owns the v4l2_device and media_device
 *   - Uses async notifier to bind the sensor subdev
 *   - Provides VB2 queue and video_device for capture
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_bridge.ko
 *   3. insmod vsoc_sensor.ko   (triggers async .complete)
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-async.h>
#include <media/videobuf2-vmalloc.h>
#include <media/media-device.h>

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
	struct media_device	mdev;		/* NEW: media device */
	struct video_device	vdev;
	struct vb2_queue	queue;
	struct mutex		lock;

	/* Async notifier for sensor */
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev	*sensor_sd;

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
 * Format Helpers
 * ==================================================================== */

static void vsoc_bridge_set_default_format(struct vsoc_bridge *bridge)
{
	struct v4l2_pix_format *pix = &bridge->pix_fmt;

	pix->width       = VSOC_SENSOR_DEF_WIDTH;
	pix->height      = VSOC_SENSOR_DEF_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_SRGGB10;
	pix->field       = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 2; /* 10-bit in 16-bit container */
	pix->sizeimage   = pix->bytesperline * pix->height;
	pix->colorspace  = V4L2_COLORSPACE_RAW;
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
 * vsoc_bridge_start_streaming — Part 8
 *
 * Step 1: Set bridge->streaming = true
 * Step 2: Reset frame sequence counter
 * Step 3: Call v4l2_subdev_call(sensor, video, s_stream, 1)
 * Step 4: [CHANGED] Start frame simulation timer (replaces DMA ring)
 *
 * Changes from Part 6/7:
 *   ~ Replaced DMA descriptor ring + ISR with timer-based simulation
 *   ~ Simplified: no ring allocation, no interrupt unmasking
 */
static int vsoc_bridge_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vq);
	int ret;

	/* Step 2: Reset frame sequence counter */
	bridge->sequence = 0;

	/* Step 3: Start sensor — v4l2_subdev_call(s_stream, 1) */
	ret = v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 1);
	if (ret && ret != -ENOIOCTLCMD)
		goto err_return_bufs;

	/* Step 1: Set bridge->streaming = true */
	/* Step 4: Start frame simulation timer (replaces DMA ring) */
	bridge->streaming = true;
	mod_timer(&bridge->frame_timer,
		  jiffies + msecs_to_jiffies(VSOC_HW_FRAME_MS));

	return 0;

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
 * vsoc_bridge_stop_streaming — Part 8
 *
 * Step 1: Set bridge->streaming = false
 * Step 2: [CHANGED] Stop frame timer (del_timer_sync)
 * Step 3: Call v4l2_subdev_call(sensor, video, s_stream, 0)
 * Step 4: Return all pending buffers to VB2
 *
 * Changes from Part 6/7:
 *   ~ Replaced work queue flush with timer stop
 */
static void vsoc_bridge_stop_streaming(struct vb2_queue *vq)
{
	struct vsoc_bridge *bridge = vb2_get_drv_priv(vq);
	struct vsoc_bridge_buffer *buf, *tmp;
	unsigned long flags;

	/* Step 1: Set bridge->streaming = false */
	bridge->streaming = false;
	/* Step 2: Stop frame timer (del_timer_sync) */
	timer_delete_sync(&bridge->frame_timer);

	/* Step 3: Stop sensor — s_stream(0) */
	v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 0);

	/* Step 4: Return all pending buffers to VB2 */
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
 * V4L2 ioctl ops
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

	f->pixelformat = V4L2_PIX_FMT_SRGGB10;
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

	pix->pixelformat  = V4L2_PIX_FMT_SRGGB10;
	pix->width  = clamp_t(u32, pix->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	pix->height = clamp_t(u32, pix->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	pix->width  = rounddown(pix->width, VSOC_SENSOR_STEP);
	pix->height = rounddown(pix->height, VSOC_SENSOR_STEP);
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 2;
	pix->sizeimage    = pix->bytesperline * pix->height;
	pix->colorspace   = V4L2_COLORSPACE_RAW;

	bridge->pix_fmt = *pix;
	return 0;
}

static int vsoc_bridge_try_fmt(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->pixelformat  = V4L2_PIX_FMT_SRGGB10;
	pix->width  = clamp_t(u32, pix->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	pix->height = clamp_t(u32, pix->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	pix->width  = rounddown(pix->width, VSOC_SENSOR_STEP);
	pix->height = rounddown(pix->height, VSOC_SENSOR_STEP);
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 2;
	pix->sizeimage    = pix->bytesperline * pix->height;
	pix->colorspace   = V4L2_COLORSPACE_RAW;

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
 * vsoc_bridge_notify_complete — Part 8
 *
 * Step 1: Register video_device — /dev/videoN appears
 * Step 2: [NEW] Register media_device — /dev/mediaN appears
 * Step 3: Set video_registered = true
 *
 * Changes from Part 7:
 *   + Added media_device_register (Step 2) — topology now visible
 *   ~ VB2 queue init moved to probe (no longer here)
 */
static int vsoc_bridge_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);
	int ret;

	/* Step 1: Register video_device — /dev/videoN appears */
	ret = video_register_device(&bridge->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"video_register_device failed: %d\n", ret);
		return ret;
	}

	/* Step 2: Register media_device — /dev/mediaN appears */
	ret = media_device_register(&bridge->mdev);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"media_device_register failed: %d\n", ret);
		video_unregister_device(&bridge->vdev);
		return ret;
	}

	dev_info(&bridge->pdev->dev,
		 "pipeline complete: /dev/video%d + /dev/media%d\n",
		 bridge->vdev.num, bridge->mdev.devnode->minor);
	return 0;
}

static const struct v4l2_async_notifier_operations vsoc_bridge_notify_ops = {
	.bound    = vsoc_bridge_notify_bound,
	.complete = vsoc_bridge_notify_complete,
};

/* ====================================================================
 * Platform Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_bridge_probe — Part 8: Media Controller Basics
 *
 * Step 1: Allocate bridge state
 * Step 2: [NEW] Initialize media_device (media_device_init)
 * Step 3: Register v4l2_device
 * Step 4: [NEW] Link media_device to v4l2_device (v4l2_dev.mdev = &mdev)
 *         CRITICAL: must be set BEFORE any subdev registration
 * Step 5: [CHANGED] Initialize VB2 queue in probe 
 * Step 6: Set up video_device basics
 * Step 7: Initialize async notifier and register
 *
 * Changes from Part 7:
 *   + Added media_device init and linkage (Steps 2, 4)
 *   ~ Switched from real DMA (ISR+ring) to timer-based frame simulation
 *   ~ VB2 queue init moved back to probe (simpler lifecycle)
 *   - Removed DMA register mapping and IRQ request
 */
static int vsoc_bridge_probe(struct platform_device *pdev)
{
	struct v4l2_async_connection *asc;
	struct vsoc_bridge *bridge;
	struct i2c_adapter *adap;
	struct vb2_queue *vq;
	int ret;

	/* Step 1: Allocate bridge state */
	bridge = devm_kzalloc(&pdev->dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	bridge->pdev = pdev;
	platform_set_drvdata(pdev, bridge);
	mutex_init(&bridge->lock);
	spin_lock_init(&bridge->buf_lock);
	INIT_LIST_HEAD(&bridge->buf_list);
	timer_setup(&bridge->frame_timer, vsoc_bridge_frame_timer, 0);

	/* Set default pixel format */
	vsoc_bridge_set_default_format(bridge);

	/* Step 2: Initialize media_device (media_device_init) */
	strscpy(bridge->mdev.model, "VSOC-3000 Camera",
		sizeof(bridge->mdev.model));
	snprintf(bridge->mdev.bus_info, sizeof(bridge->mdev.bus_info),
		 "platform:%s", dev_name(&pdev->dev));
	bridge->mdev.dev = &pdev->dev;
	bridge->mdev.hw_revision = 0x0100;
	media_device_init(&bridge->mdev);

	/* Step 3: Register v4l2_device */
	/* Step 4: Link media_device to v4l2_device (must be before subdev reg) */
	bridge->v4l2_dev.mdev = &bridge->mdev;
	ret = v4l2_device_register(&pdev->dev, &bridge->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "v4l2_device_register failed: %d\n", ret);
		goto err_media_cleanup;
	}

	/* Step 5: Initialize VB2 queue in probe (simplified from Part 7) */
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

	/* Step 6: Set up video_device basics */
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

	/* Step 7: Initialize async notifier and register */
	adap = vsoc_hw_get_i2c_adapter();
	if (!adap) {
		dev_err(&pdev->dev, "I2C adapter not available\n");
		ret = -ENODEV;
		goto err_v4l2;
	}

	v4l2_async_nf_init(&bridge->notifier, &bridge->v4l2_dev);
	bridge->notifier.ops = &vsoc_bridge_notify_ops;

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

	ret = v4l2_async_nf_register(&bridge->notifier);
	if (ret) {
		dev_err(&pdev->dev,
			"v4l2_async_nf_register failed: %d\n", ret);
		goto err_nf_cleanup;
	}

	dev_info(&pdev->dev, "vsoc_bridge probed (waiting for sensor)\n");
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

	/* NEW: media device teardown */
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
MODULE_DESCRIPTION("VSOC-3000 Camera Bridge -- Part 8: Media Controller Basics");
MODULE_VERSION("1.0.0");
