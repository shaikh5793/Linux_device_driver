// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Bridge/Capture Driver — Part 7: Async Subdev Binding
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPTS:
 *   - v4l2_async_notifier for load-order-independent subdev binding
 *   - v4l2_async_nf_add_i2c() to match sensor by I2C bus + address
 *   - Notifier .bound / .complete / .unbind callbacks
 *   - Video device registration deferred to .complete callback
 *
 * Load order (either works):
 *   A. insmod soc_hw_platform.ko -> vsoc_sensor.ko -> vsoc_bridge.ko
 *   B. insmod soc_hw_platform.ko -> vsoc_bridge.ko -> vsoc_sensor.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME	"vsoc_bridge"
#define RING_SIZE	8

/* ====================================================================
 * Data Structures
 * ==================================================================== */

struct vsoc_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct vsoc_bridge {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;		/* serialize ioctls */

	struct v4l2_subdev *sensor_sd;
	struct v4l2_ctrl_handler ctrl_handler;

	/* Async notifier — NEW in Part 7 */
	struct v4l2_async_notifier notifier;

	/* DMA engine */
	void __iomem *dma_regs;
	int irq;

	/* Descriptor ring */
	struct vsoc_hw_desc *ring;
	struct vsoc_buffer *ring_bufs[RING_SIZE];
	unsigned int ring_head;
	unsigned int ring_tail;

	/* IRQ bottom half */
	struct work_struct irq_work;

	/* Pending buffer list */
	struct list_head buf_list;
	spinlock_t buf_lock;

	/* Format */
	struct v4l2_pix_format fmt;
	unsigned int sequence;

	struct platform_device *pdev;
	bool video_registered;
};

static inline struct vsoc_buffer *to_vsoc_buffer(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct vsoc_buffer, vb);
}

/* ====================================================================
 * DMA Descriptor Ring Helpers
 * ==================================================================== */

static void vsoc_bridge_submit_buffer(struct vsoc_bridge *bridge,
				      struct vsoc_buffer *buf)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	unsigned int idx = bridge->ring_head & (RING_SIZE - 1);
	struct vsoc_hw_desc *desc = &bridge->ring[idx];
	void *vaddr;

	vaddr = vb2_plane_vaddr(vb, 0);

	desc->addr_lo = lower_32_bits((unsigned long)vaddr);
	desc->addr_hi = upper_32_bits((unsigned long)vaddr);
	desc->size = vb2_plane_size(vb, 0);
	desc->flags = VSOC_DESC_OWN;

	bridge->ring_bufs[idx] = buf;
	bridge->ring_head++;

	/* Write head pointer to DMA doorbell */
	iowrite32(bridge->ring_head, bridge->dma_regs + VSOC_DMA_BUF_RING_HEAD);
}

static void vsoc_bridge_drain_pending(struct vsoc_bridge *bridge)
{
	struct vsoc_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&bridge->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &bridge->buf_list, list) {
		list_del(&buf->list);
		vsoc_bridge_submit_buffer(bridge, buf);
	}
	spin_unlock_irqrestore(&bridge->buf_lock, flags);
}

/* ====================================================================
 * ISR and IRQ Work
 * ==================================================================== */

static irqreturn_t vsoc_bridge_isr(int irq, void *data)
{
	struct vsoc_bridge *bridge = data;
	u32 status;

	status = ioread32(bridge->dma_regs + VSOC_DMA_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	/* Acknowledge interrupts */
	iowrite32(status, bridge->dma_regs + VSOC_DMA_INT_STATUS);

	if (status & VSOC_DMA_INT_FRAME_DONE)
		schedule_work(&bridge->irq_work);

	return IRQ_HANDLED;
}

static void vsoc_bridge_irq_work(struct work_struct *work)
{
	struct vsoc_bridge *bridge =
		container_of(work, struct vsoc_bridge, irq_work);
	unsigned int hw_tail;
	unsigned int idx;

	hw_tail = ioread32(bridge->dma_regs + VSOC_DMA_BUF_RING_TAIL);

	while (bridge->ring_tail != hw_tail) {
		idx = bridge->ring_tail & (RING_SIZE - 1);

		if (bridge->ring_bufs[idx]) {
			struct vsoc_buffer *buf = bridge->ring_bufs[idx];
			struct vsoc_hw_desc *desc = &bridge->ring[idx];

			buf->vb.vb2_buf.timestamp = ktime_get_ns();
			buf->vb.sequence = bridge->sequence++;
			buf->vb.field = V4L2_FIELD_NONE;

			if (desc->flags & VSOC_DESC_ERROR)
				vb2_buffer_done(&buf->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
			else
				vb2_buffer_done(&buf->vb.vb2_buf,
						VB2_BUF_STATE_DONE);

			bridge->ring_bufs[idx] = NULL;
		}

		bridge->ring_tail++;
	}
}

/* ====================================================================
 * VB2 Queue Operations
 * ==================================================================== */

static int vsoc_bridge_queue_setup(struct vb2_queue *q,
				   unsigned int *nbuffers,
				   unsigned int *nplanes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct vsoc_bridge *bridge =
		container_of(q, struct vsoc_bridge, queue);
	unsigned int size = bridge->fmt.sizeimage;

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int vsoc_bridge_buf_prepare(struct vb2_buffer *vb)
{
	struct vsoc_bridge *bridge =
		container_of(vb->vb2_queue, struct vsoc_bridge, queue);
	unsigned int size = bridge->fmt.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&bridge->pdev->dev,
			"buffer too small (%lu < %u)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void vsoc_bridge_buf_queue(struct vb2_buffer *vb)
{
	struct vsoc_bridge *bridge =
		container_of(vb->vb2_queue, struct vsoc_bridge, queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vsoc_buffer *buf = to_vsoc_buffer(vbuf);
	unsigned long flags;

	if (bridge->ring) {
		vsoc_bridge_submit_buffer(bridge, buf);
	} else {
		spin_lock_irqsave(&bridge->buf_lock, flags);
		list_add_tail(&buf->list, &bridge->buf_list);
		spin_unlock_irqrestore(&bridge->buf_lock, flags);
	}
}

/*
 * vsoc_bridge_start_streaming — Part 7
 *
 * Identical to Part 6. Uses real DMA descriptor ring + ISR.
 * No changes from Part 6 — async binding only affects probe path.
 */
static int vsoc_bridge_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vsoc_bridge *bridge =
		container_of(q, struct vsoc_bridge, queue);
	int ret;

	bridge->sequence = 0;
	bridge->ring_head = 0;
	bridge->ring_tail = 0;

	/* Step 1: Allocate DMA descriptor ring (RING_SIZE entries) */
	bridge->ring = kcalloc(RING_SIZE, sizeof(struct vsoc_hw_desc),
			       GFP_KERNEL);
	if (!bridge->ring) {
		ret = -ENOMEM;
		goto err_return_bufs;
	}

	memset(bridge->ring_bufs, 0, sizeof(bridge->ring_bufs));

	/* Step 2: Register ring with hardware (vsoc_hw_set_buf_ring) */
	ret = vsoc_hw_set_buf_ring(bridge->ring, RING_SIZE);
	if (ret)
		goto err_free_ring;

	/* Step 4: Submit all queued buffers to ring */
	vsoc_bridge_drain_pending(bridge);

	/* Step 3: Program DMA format registers (width, height, format, stride) */
	iowrite32(bridge->fmt.width,
		  bridge->dma_regs + VSOC_DMA_FMT_WIDTH);
	iowrite32(bridge->fmt.height,
		  bridge->dma_regs + VSOC_DMA_FMT_HEIGHT);
	iowrite32(bridge->fmt.bytesperline,
		  bridge->dma_regs + VSOC_DMA_FMT_STRIDE);
	iowrite32(bridge->fmt.sizeimage,
		  bridge->dma_regs + VSOC_DMA_FMT_FRAMESIZE);

	/* Step 5: Unmask frame-done interrupt */
	iowrite32(VSOC_DMA_INT_FRAME_DONE,
		  bridge->dma_regs + VSOC_DMA_INT_MASK);

	/* Step 6: Start sensor — v4l2_subdev_call(s_stream, 1) */
	ret = v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 1);
	if (ret)
		goto err_clear_ring;

	/* Step 7: Notify hardware to start (vsoc_hw_notify_stream) */
	vsoc_hw_notify_stream(1);

	dev_info(&bridge->pdev->dev, "streaming started (%ux%u)\n",
		 bridge->fmt.width, bridge->fmt.height);
	return 0;

err_clear_ring:
	iowrite32(0, bridge->dma_regs + VSOC_DMA_INT_MASK);
	vsoc_hw_clear_buf_ring();
err_free_ring:
	kfree(bridge->ring);
	bridge->ring = NULL;
err_return_bufs:
	{
		struct vsoc_buffer *buf, *tmp;
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
 * vsoc_bridge_stop_streaming — Part 7
 *
 * Identical to Part 6. No changes — async binding only affects probe path.
 */
static void vsoc_bridge_stop_streaming(struct vb2_queue *q)
{
	struct vsoc_bridge *bridge =
		container_of(q, struct vsoc_bridge, queue);
	struct vsoc_buffer *buf, *tmp;
	unsigned long flags;
	unsigned int i;

	/* Step 1: Notify hardware to stop */
	vsoc_hw_notify_stream(0);

	/* Step 2: Stop sensor — s_stream(0) */
	v4l2_subdev_call(bridge->sensor_sd, video, s_stream, 0);

	/* Step 3: Mask DMA interrupts */
	iowrite32(0, bridge->dma_regs + VSOC_DMA_INT_MASK);

	/* Step 4: Flush work queue (cancel_work_sync) */
	cancel_work_sync(&bridge->irq_work);

	/* Step 5: Return all pending buffers to VB2 (VB2_BUF_STATE_ERROR) */
	for (i = 0; i < RING_SIZE; i++) {
		if (bridge->ring_bufs[i]) {
			vb2_buffer_done(&bridge->ring_bufs[i]->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			bridge->ring_bufs[i] = NULL;
		}
	}

	/* Step 5 (cont): Return all pending list buffers */
	spin_lock_irqsave(&bridge->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &bridge->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&bridge->buf_lock, flags);

	/* Step 6: Free DMA descriptor ring */
	vsoc_hw_clear_buf_ring();
	kfree(bridge->ring);
	bridge->ring = NULL;

	dev_info(&bridge->pdev->dev, "streaming stopped\n");
}

static const struct vb2_ops vsoc_bridge_vb2_ops = {
	.queue_setup     = vsoc_bridge_queue_setup,
	.buf_prepare     = vsoc_bridge_buf_prepare,
	.buf_queue       = vsoc_bridge_buf_queue,
	.start_streaming = vsoc_bridge_start_streaming,
	.stop_streaming  = vsoc_bridge_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ====================================================================
 * V4L2 IOCTL Operations
 * ==================================================================== */

static int vsoc_bridge_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VSOC-3000 Bridge (async)", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vsoc_bridge", sizeof(cap->bus_info));
	return 0;
}

static int vsoc_bridge_enum_fmt(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_RGB24;
	return 0;
}

static int vsoc_bridge_g_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct vsoc_bridge *bridge = video_drvdata(file);

	f->fmt.pix = bridge->fmt;
	return 0;
}

static void vsoc_bridge_try_fmt_helper(struct v4l2_pix_format *pix)
{
	pix->pixelformat = V4L2_PIX_FMT_RGB24;

	pix->width = clamp_t(u32, pix->width,
			     VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	pix->width = rounddown(pix->width, VSOC_SENSOR_STEP);
	pix->height = clamp_t(u32, pix->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	pix->height = rounddown(pix->height, VSOC_SENSOR_STEP);

	pix->bytesperline = pix->width * 3;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
}

static int vsoc_bridge_try_fmt(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	vsoc_bridge_try_fmt_helper(&f->fmt.pix);
	return 0;
}

static int vsoc_bridge_s_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct vsoc_bridge *bridge = video_drvdata(file);

	if (vb2_is_busy(&bridge->queue))
		return -EBUSY;

	vsoc_bridge_try_fmt_helper(&f->fmt.pix);
	bridge->fmt = f->fmt.pix;
	return 0;
}

static const struct v4l2_ioctl_ops vsoc_bridge_ioctl_ops = {
	.vidioc_querycap          = vsoc_bridge_querycap,
	.vidioc_enum_fmt_vid_cap  = vsoc_bridge_enum_fmt,
	.vidioc_g_fmt_vid_cap     = vsoc_bridge_g_fmt,
	.vidioc_s_fmt_vid_cap     = vsoc_bridge_s_fmt,
	.vidioc_try_fmt_vid_cap   = vsoc_bridge_try_fmt,

	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,
	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,

	.vidioc_log_status        = v4l2_ctrl_log_status,
	.vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ====================================================================
 * V4L2 File Operations
 * ==================================================================== */

static const struct v4l2_file_operations vsoc_bridge_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.mmap           = vb2_fop_mmap,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
};

/* ====================================================================
 * Async Notifier Callbacks — NEW in Part 7
 * ==================================================================== */

/*
 * vsoc_bridge_nf_bound — Part 7: Async .bound callback
 *
 * Called by the async framework when a matching subdev is found.
 *
 * Step 1: Store sensor subdev pointer for later use
 * Step 2: Initialize bridge ctrl_handler
 * Step 3: Inherit sensor controls via v4l2_ctrl_add_handler
 * Step 4: Link ctrl_handler to video_device
 *
 * First appearance in Part 7.
 */
static int vsoc_bridge_nf_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *sd,
				struct v4l2_async_connection *asc)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);
	int ret;

	/* Step 1: Store sensor subdev pointer for later use */
	bridge->sensor_sd = sd;
	dev_info(&bridge->pdev->dev, "sensor subdev '%s' bound\n", sd->name);

	/* Step 2: Initialize bridge ctrl_handler */
	ret = v4l2_ctrl_handler_init(&bridge->ctrl_handler, 0);
	if (ret)
		return ret;

	/* Step 3: Inherit sensor controls via v4l2_ctrl_add_handler */
	ret = v4l2_ctrl_add_handler(&bridge->ctrl_handler,
				    sd->ctrl_handler, NULL, true);
	if (ret) {
		v4l2_ctrl_handler_free(&bridge->ctrl_handler);
		return ret;
	}

	return 0;
}

/*
 * vsoc_bridge_nf_complete — Part 7: Async .complete callback
 *
 * Called when ALL expected subdevs are bound. Safe to expose to userspace.
 *
 * Step 1: Initialize VB2 queue (MMAP, vb2_vmalloc_memops)
 * Step 2: Set up video_device with ioctl_ops and vb2 queue
 * Step 3: Register video_device — /dev/videoN now appears
 * Step 4: Set video_registered = true
 *
 * First appearance in Part 7. Video device deferred from probe to here.
 */
static int vsoc_bridge_nf_complete(struct v4l2_async_notifier *notifier)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);
	struct vb2_queue *q;
	int ret;

	/* Step 1: Initialize VB2 queue (MMAP, vb2_vmalloc_memops) */
	q = &bridge->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	q->drv_priv = bridge;
	q->ops = &vsoc_bridge_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct vsoc_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &bridge->lock;
	q->min_queued_buffers = 2;

	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	/* Step 2: Set up video_device with ioctl_ops and vb2 queue */
	strscpy(bridge->vdev.name, "VSOC-3000 Capture (async)",
		sizeof(bridge->vdev.name));
	bridge->vdev.fops = &vsoc_bridge_fops;
	bridge->vdev.ioctl_ops = &vsoc_bridge_ioctl_ops;
	bridge->vdev.release = video_device_release_empty;
	bridge->vdev.v4l2_dev = &bridge->v4l2_dev;
	bridge->vdev.queue = &bridge->queue;
	bridge->vdev.lock = &bridge->lock;
	bridge->vdev.ctrl_handler = &bridge->ctrl_handler;
	bridge->vdev.vfl_type = VFL_TYPE_VIDEO;
	bridge->vdev.vfl_dir = VFL_DIR_RX;
	bridge->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				   V4L2_CAP_STREAMING |
				   V4L2_CAP_READWRITE;
	video_set_drvdata(&bridge->vdev, bridge);

	/* Step 3: Register video_device — /dev/videoN now appears */
	ret = video_register_device(&bridge->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&bridge->pdev->dev,
			"failed to register video device: %d\n", ret);
		vb2_queue_release(&bridge->queue);
		return ret;
	}

	/* Step 4: Set video_registered = true */
	bridge->video_registered = true;
	dev_info(&bridge->pdev->dev,
		 "VSOC-3000 bridge registered as /dev/video%d (async complete)\n",
		 bridge->vdev.num);
	return 0;
}

/*
 * vsoc_bridge_nf_unbind — Part 7: Async .unbind callback
 *
 * Called when a bound subdev is removed (e.g., rmmod sensor).
 *
 * Step 1: Unregister video_device if it was registered
 * Step 2: Free ctrl_handler
 * Step 3: Clear sensor_sd pointer
 * Step 4: Set video_registered = false
 *
 * First appearance in Part 7.
 */
static void vsoc_bridge_nf_unbind(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *sd,
				  struct v4l2_async_connection *asc)
{
	struct vsoc_bridge *bridge =
		container_of(notifier, struct vsoc_bridge, notifier);

	/* Step 1: Unregister video_device if it was registered */
	if (bridge->video_registered) {
		video_unregister_device(&bridge->vdev);
		vb2_queue_release(&bridge->queue);
		/* Step 4: Set video_registered = false */
		bridge->video_registered = false;
	}

	/* Step 2: Free ctrl_handler */
	v4l2_ctrl_handler_free(&bridge->ctrl_handler);
	/* Step 3: Clear sensor_sd pointer */
	bridge->sensor_sd = NULL;

	dev_info(&bridge->pdev->dev, "sensor subdev '%s' unbound\n",
		 sd->name);
}

static const struct v4l2_async_notifier_operations vsoc_bridge_nf_ops = {
	.bound    = vsoc_bridge_nf_bound,
	.complete = vsoc_bridge_nf_complete,
	.unbind   = vsoc_bridge_nf_unbind,
};

/* ====================================================================
 * Platform Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_bridge_probe — Part 7: Async Subdev Binding
 *
 * Step 1: Allocate bridge state
 * Step 2: Map DMA MMIO registers and request IRQ
 * Step 3: Register v4l2_device
 * Step 4: [REMOVED] No sensor discovery here — deferred to async
 * Step 5: [REMOVED] No VB2/video_device setup here — deferred to .complete
 * Step 6: [NEW] Initialize async notifier (v4l2_async_nf_init)
 * Step 7: [NEW] Add I2C match entry (adapter 0, address 0x10)
 * Step 8: [NEW] Register async notifier — framework will call .bound/.complete
 *
 * Changes from Part 6:
 *   - Removed synchronous sensor lookup (find_sensor)
 *   - Removed video_device registration (moved to .complete)
 *   - Removed VB2 queue init (moved to .complete)
 *   + Added async notifier setup (Steps 6-8)
 *   Video device now appears ONLY when all subdevs are bound.
 */
static int vsoc_bridge_probe(struct platform_device *pdev)
{
	struct vsoc_bridge *bridge;
	struct v4l2_async_connection *asc;
	struct i2c_adapter *adap;
	int ret;

	/* Step 1: Allocate bridge state */
	bridge = devm_kzalloc(&pdev->dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	bridge->pdev = pdev;
	mutex_init(&bridge->lock);
	spin_lock_init(&bridge->buf_lock);
	INIT_LIST_HEAD(&bridge->buf_list);
	INIT_WORK(&bridge->irq_work, vsoc_bridge_irq_work);

	/* Set default format: 1920x1080 RGB24 */
	bridge->fmt.width = 1920;
	bridge->fmt.height = 1080;
	bridge->fmt.pixelformat = V4L2_PIX_FMT_RGB24;
	bridge->fmt.field = V4L2_FIELD_NONE;
	bridge->fmt.colorspace = V4L2_COLORSPACE_SRGB;
	bridge->fmt.bytesperline = 1920 * 3;
	bridge->fmt.sizeimage = 1920 * 3 * 1080;

	/* Step 2: Map DMA MMIO registers and request IRQ */
	bridge->dma_regs = vsoc_hw_map_dma_regs();
	if (!bridge->dma_regs) {
		dev_err(&pdev->dev, "failed to map DMA registers\n");
		return -ENOMEM;
	}

	/* Step 2 (cont): Get and request IRQ */
	bridge->irq = vsoc_hw_get_dma_irq();
	if (bridge->irq < 0) {
		dev_err(&pdev->dev, "failed to get DMA IRQ\n");
		ret = bridge->irq;
		goto err_unmap_dma;
	}

	ret = request_irq(bridge->irq, vsoc_bridge_isr, IRQF_SHARED,
			  DRV_NAME, bridge);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ %d: %d\n",
			bridge->irq, ret);
		goto err_unmap_dma;
	}

	/* Step 3: Register v4l2_device */
	ret = v4l2_device_register(&pdev->dev, &bridge->v4l2_dev);
	if (ret)
		goto err_free_irq;

	/*
	 * Step 6: Initialize async notifier (v4l2_async_nf_init)
	 *
	 * How a real SoC bridge discovers its sensor — Device Tree
	 * --------------------------------------------------------
	 * On real hardware, the bridge driver is a platform driver and the
	 * sensor is an I2C driver — two completely separate bus stacks.
	 * The bridge has no business knowing the I2C bus number or slave
	 * address.  Instead, the Device Tree describes the connection via
	 * an of_graph port/endpoint:
	 *
	 *   // Device Tree snippet (e.g. imx8-isi, rkisp1, bcm2835-unicam)
	 *   bridge@58000000 {
	 *       compatible = "vendor,soc-camera";
	 *       port {
	 *           bridge_in: endpoint {
	 *               remote-endpoint = <&sensor_out>;
	 *           };
	 *       };
	 *   };
	 *
	 *   &i2c2 {
	 *       sensor@10 {
	 *           compatible = "vendor,imx219";
	 *           reg = <0x10>;
	 *           port {
	 *               sensor_out: endpoint {
	 *                   remote-endpoint = <&bridge_in>;
	 *               };
	 *           };
	 *       };
	 *   };
	 *
	 * At probe, the bridge walks its of_graph endpoints and registers
	 * each remote fwnode with the async framework:
	 *
	 *   struct fwnode_handle *ep, *remote;
	 *
	 *   v4l2_async_nf_init(&notifier, &v4l2_dev);
	 *
	 *   fwnode_graph_for_each_endpoint(dev_fwnode(&pdev->dev), ep) {
	 *       remote = fwnode_graph_get_remote_port_parent(ep);
	 *       asc = v4l2_async_nf_add_fwnode(&notifier, remote,
	 *                                      struct v4l2_async_connection);
	 *       fwnode_handle_put(remote);
	 *   }
	 *
	 *   v4l2_async_nf_register(&notifier);
	 *
	 * The bridge never touches I2C — it passes an opaque fwnode to the
	 * V4L2 async framework.  The framework matches it against the
	 * sensor's fwnode (set by v4l2_i2c_subdev_init → sd->fwnode) and
	 * fires the .bound callback.  The bridge doesn't know or care
	 * whether the sensor is I2C, SPI, or platform.
	 *
	 * What we do here instead (virtual hardware — no Device Tree)
	 * -----------------------------------------------------------
	 * Since soc_hw_platform.ko is a software-only simulation with no
	 * DT nodes, we use the I2C match variant: we ask the platform
	 * module for the adapter handle (vsoc_hw_get_i2c_adapter) and
	 * pass the bus number + hardcoded address to v4l2_async_nf_add_i2c.
	 * This is functionally equivalent but couples the bridge to the
	 * I2C bus — something a production driver should never do.
	 */
	adap = vsoc_hw_get_i2c_adapter();
	if (!adap) {
		dev_err(&pdev->dev, "I2C adapter not available\n");
		ret = -ENODEV;
		goto err_v4l2_unreg;
	}

	v4l2_async_nf_init(&bridge->notifier, &bridge->v4l2_dev);

	/* Step 7: Add I2C match entry (adapter, address 0x10) */
	asc = v4l2_async_nf_add_i2c(&bridge->notifier,
				     i2c_adapter_id(adap),
				     VSOC_SENSOR_I2C_ADDR,
				     struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		dev_err(&pdev->dev, "failed to add async I2C match: %ld\n",
			PTR_ERR(asc));
		ret = PTR_ERR(asc);
		goto err_nf_cleanup;
	}

	bridge->notifier.ops = &vsoc_bridge_nf_ops;

	/* Step 8: Register async notifier */
	ret = v4l2_async_nf_register(&bridge->notifier);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register async notifier: %d\n", ret);
		goto err_nf_cleanup;
	}

	dev_info(&pdev->dev,
		 "VSOC-3000 bridge probed (waiting for async sensor match)\n");
	return 0;

err_nf_cleanup:
	v4l2_async_nf_cleanup(&bridge->notifier);
err_v4l2_unreg:
	v4l2_device_unregister(&bridge->v4l2_dev);
err_free_irq:
	free_irq(bridge->irq, bridge);
err_unmap_dma:
	vsoc_hw_unmap_dma_regs();
	return ret;
}

/*
 * vsoc_bridge_remove — Part 7
 *
 * Step 1: Conditionally unregister video_device (if video_registered)
 * Step 2: [NEW] Unregister async notifier
 * Step 3: [NEW] Clean up async notifier resources
 * Step 4: Unregister v4l2_device
 * Step 5: Free IRQ and unmap DMA registers
 *
 * Changes from Part 6:
 *   + Added async notifier cleanup (Steps 2-3)
 *   ~ Video unregister now conditional on video_registered flag
 */
static void vsoc_bridge_remove(struct platform_device *pdev)
{
	struct v4l2_device *v4l2_dev = platform_get_drvdata(pdev);
	struct vsoc_bridge *bridge =
		container_of(v4l2_dev, struct vsoc_bridge, v4l2_dev);

	/* Step 2: Unregister async notifier */
	v4l2_async_nf_unregister(&bridge->notifier);
	/* Step 3: Clean up async notifier resources */
	v4l2_async_nf_cleanup(&bridge->notifier);

	/* Step 1: Conditionally unregister video_device */
	if (bridge->video_registered) {
		video_unregister_device(&bridge->vdev);
		vb2_queue_release(&bridge->queue);
		v4l2_ctrl_handler_free(&bridge->ctrl_handler);
	}

	/* Step 4: Unregister v4l2_device */
	v4l2_device_unregister(&bridge->v4l2_dev);
	/* Step 5: Free IRQ and unmap DMA registers */
	free_irq(bridge->irq, bridge);
	vsoc_hw_unmap_dma_regs();
	dev_info(&pdev->dev, "VSOC-3000 bridge removed\n");
}

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
MODULE_DESCRIPTION("VSOC-3000 Bridge/Capture — Part 7: Async Subdev Binding");
MODULE_VERSION("1.0.0");
