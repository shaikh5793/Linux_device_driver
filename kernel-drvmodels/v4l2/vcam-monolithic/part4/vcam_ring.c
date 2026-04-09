// SPDX-License-Identifier: GPL-2.0
/*
 * Part 4: V4L2 Capture Device with Descriptor Ring DMA
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Concepts introduced (over Part 3):
 *   - Hardware descriptor ring for frame delivery
 *   - Ring buffer head/tail management with OWN flag handshake
 *   - DMA-style buffer submission (vcam_submit_buffer)
 *   - Producer-consumer model: driver writes descriptors, HW consumes
 *   - Polled completion via delayed work (reads TAIL register)
 *
 * Carries forward from Part 3:
 *   - VB2 buffer management and streaming callbacks
 *   - Timer-based frame delivery (now with ring polling)
 *   - Format negotiation, V4L2/video_device registration
 *
 * NOT yet covered (see Part 5):
 *   - Interrupt-driven frame completion (request_irq)
 *   - ISR top-half / bottom-half split
 *
 * Design note:
 *   Part 3 used a simple list + timer.  This part replaces the list
 *   with a hardware descriptor ring — the same DMA programming
 *   pattern used by real camera ISPs, network cards, and storage
 *   controllers.  Completion is still polled via a timer; Part 5
 *   upgrades to interrupt-driven completion.
 *
 * LOAD ORDER:
 *   sudo insmod ../hw/vcam_hw_platform.ko
 *   sudo insmod vcam_ring.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../hw/vcam_hw_interface.h"

#define DRV_NAME	"vcam_ring"
#define VCAM_RING_SIZE	8

struct vcam_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head       list;	/* driver-internal queued list */
};

struct vcam_device {
	struct v4l2_device     v4l2_dev;
	struct video_device    vdev;
	struct vb2_queue       queue;
	struct v4l2_pix_format fmt;
	struct mutex           lock;
	struct platform_device *pdev;

	/* Hardware interface */
	void __iomem          *regs;

	/* Driver-internal buffer list (buf_queue → start_streaming hand-off) */
	struct list_head       buf_list;
	spinlock_t             buf_lock;

	/* Descriptor ring */
	struct vcam_hw_desc   *ring;
	struct vcam_buffer   **ring_bufs;
	u32                    ring_head;
	u32                    ring_tail;

	/* Polled completion via delayed work */
	struct delayed_work    poll_work;
	unsigned int           sequence;
};

/* ---- Descriptor ring helpers ---- */

/*
 * vcam_submit_buffer -- Submit a VB2 buffer to the hardware descriptor ring
 *
 * This is the core DMA programming routine. In a real camera driver:
 *   1. The buffer's DMA address would come from vb2_dma_contig_plane_dma_addr()
 *   2. The address would be a physical address the DMA engine can reach
 *   3. The OWN flag tells hardware "this buffer is yours to fill"
 *   4. Writing HEAD is the "doorbell" — it wakes up the DMA engine
 *
 * The ring operates as a producer-consumer queue:
 *   Driver (producer): fills descriptors, advances HEAD
 *   Hardware (consumer): reads descriptors, advances TAIL
 */
static void vcam_submit_buffer(struct vcam_device *vcam,
			       struct vcam_buffer *buf)
{
	struct vcam_hw_desc *desc;
	void *vaddr;
	u32 head = vcam->ring_head;

	vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

	desc = &vcam->ring[head];
	desc->addr_lo = lower_32_bits((unsigned long)vaddr);
	desc->addr_hi = upper_32_bits((unsigned long)vaddr);
	desc->size    = vcam->fmt.sizeimage;
	desc->flags   = VCAM_DESC_OWN;

	vcam->ring_bufs[head] = buf;
	vcam->ring_head = (head + 1) % VCAM_RING_SIZE;

	/* Write doorbell: tell hardware a new buffer is available */
	iowrite32(vcam->ring_head, vcam->regs + VCAM_BUF_RING_HEAD);
}

/* ---- Polled completion (timer-based) ---- */

/*
 * vcam_poll_work -- Poll hardware TAIL to detect completed frames
 *
 * In this part, we poll the hardware's TAIL register periodically
 * to check if frames have been completed.  Part 5 replaces this
 * with interrupt-driven notification.
 *
 * The algorithm:
 *   1. Read hardware TAIL pointer (how far hardware has progressed)
 *   2. Walk from driver's saved tail to hardware tail
 *   3. For each completed descriptor: timestamp, sequence, done()
 *   4. Update driver's saved tail
 *   5. Re-arm the timer
 */
static void vcam_poll_work(struct work_struct *work)
{
	struct vcam_device *vcam =
		container_of(work, struct vcam_device, poll_work.work);
	struct vcam_hw_desc *desc;
	struct vcam_buffer *buf;
	u32 hw_tail, tail;

	hw_tail = ioread32(vcam->regs + VCAM_BUF_RING_TAIL);
	tail = vcam->ring_tail;

	while (tail != hw_tail) {
		desc = &vcam->ring[tail];
		buf  = vcam->ring_bufs[tail];

		if (buf) {
			buf->vb.vb2_buf.timestamp = ktime_get_ns();
			buf->vb.sequence = vcam->sequence++;
			buf->vb.field = V4L2_FIELD_NONE;

			if (desc->flags & VCAM_DESC_ERROR)
				vb2_buffer_done(&buf->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
			else
				vb2_buffer_done(&buf->vb.vb2_buf,
						VB2_BUF_STATE_DONE);

			vcam->ring_bufs[tail] = NULL;
		}

		tail = (tail + 1) % VCAM_RING_SIZE;
	}

	vcam->ring_tail = tail;

	/* Re-arm poll timer */
	if (vcam->ring)
		schedule_delayed_work(&vcam->poll_work,
				      msecs_to_jiffies(VCAM_HW_FRAME_MS / 2));
}

/* ---- VB2 operations ---- */

static int vcam_queue_setup(struct vb2_queue *q,
			    unsigned int *num_buffers,
			    unsigned int *num_planes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct vcam_device *vcam = vb2_get_drv_priv(q);

	if (*num_buffers < 2)
		*num_buffers = 2;

	if (*num_planes)
		return sizes[0] < vcam->fmt.sizeimage ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = vcam->fmt.sizeimage;
	return 0;
}

static int vcam_buf_prepare(struct vb2_buffer *vb)
{
	struct vcam_device *vcam = vb2_get_drv_priv(vb->vb2_queue);

	vb2_set_plane_payload(vb, 0, vcam->fmt.sizeimage);
	return 0;
}

/*
 * vcam_buf_queue -- VB2 hands a buffer to the driver for filling
 *
 * IMPORTANT: VB2 calls buf_queue for all initially-queued buffers
 * BEFORE calling start_streaming.  The descriptor ring does not exist
 * yet at that point, so we must NOT touch the ring here.  Instead,
 * we add the buffer to a driver-internal list.  start_streaming()
 * drains this list into the ring after allocating it.
 *
 * Buffers queued while streaming is active are submitted directly
 * to the hardware ring.
 */
static void vcam_buf_queue(struct vb2_buffer *vb)
{
	struct vcam_device *vcam = vb2_get_drv_priv(vb->vb2_queue);
	struct vcam_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
					       struct vcam_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vcam->buf_lock, flags);
	if (vcam->ring) {
		/* Streaming active — submit directly to hardware ring */
		vcam_submit_buffer(vcam, buf);
	} else {
		/* Ring not yet allocated — park on driver list */
		list_add_tail(&buf->list, &vcam->buf_list);
	}
	spin_unlock_irqrestore(&vcam->buf_lock, flags);
}

static int vcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vcam_device *vcam = vb2_get_drv_priv(q);
	struct vcam_buffer *buf, *tmp;
	unsigned long flags;
	int ret;

	/* Allocate descriptor ring */
	vcam->ring_bufs = kcalloc(VCAM_RING_SIZE, sizeof(void *), GFP_KERNEL);
	if (!vcam->ring_bufs) {
		ret = -ENOMEM;
		goto err_return_bufs;
	}

	vcam->ring_head = 0;
	vcam->ring_tail = 0;
	vcam->sequence  = 0;

	vcam->ring = kcalloc(VCAM_RING_SIZE, sizeof(struct vcam_hw_desc),
			     GFP_KERNEL);
	if (!vcam->ring) {
		ret = -ENOMEM;
		goto err_free_bufs;
	}

	/* Register ring with hardware (resets HEAD/TAIL to 0) */
	ret = vcam_hw_set_buf_ring(vcam->ring, VCAM_RING_SIZE);
	if (ret)
		goto err_free_ring;

	/* Drain the queued buffer list into the hardware ring */
	spin_lock_irqsave(&vcam->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vcam_submit_buffer(vcam, buf);
	}
	spin_unlock_irqrestore(&vcam->buf_lock, flags);

	/* Program format into hardware */
	iowrite32(vcam->fmt.width,        vcam->regs + VCAM_FMT_WIDTH);
	iowrite32(vcam->fmt.height,       vcam->regs + VCAM_FMT_HEIGHT);
	iowrite32(vcam->fmt.pixelformat,  vcam->regs + VCAM_FMT_PIXFMT);
	iowrite32(vcam->fmt.bytesperline, vcam->regs + VCAM_FMT_STRIDE);
	iowrite32(vcam->fmt.sizeimage,    vcam->regs + VCAM_FMT_FRAMESIZE);

	/* Enable and start streaming (with ring mode) */
	iowrite32(VCAM_CTRL_ENABLE | VCAM_CTRL_STREAM_ON |
		  VCAM_CTRL_RING_ENABLE, vcam->regs + VCAM_CTRL);
	vcam_hw_notify_ctrl();

	/* Start polling for completed frames */
	schedule_delayed_work(&vcam->poll_work,
			      msecs_to_jiffies(VCAM_HW_FRAME_MS));

	pr_info(DRV_NAME ": streaming started (%u buffers, polled)\n", count);
	return 0;

err_free_ring:
	spin_lock_irqsave(&vcam->buf_lock, flags);
	kfree(vcam->ring);
	vcam->ring = NULL;
	spin_unlock_irqrestore(&vcam->buf_lock, flags);
err_free_bufs:
	kfree(vcam->ring_bufs);
	vcam->ring_bufs = NULL;
err_return_bufs:
	/* VB2 requires all buffers returned on start_streaming failure */
	spin_lock_irqsave(&vcam->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_QUEUED);
	}
	spin_unlock_irqrestore(&vcam->buf_lock, flags);
	return ret;
}

static void vcam_stop_streaming(struct vb2_queue *q)
{
	struct vcam_device *vcam = vb2_get_drv_priv(q);
	struct vcam_buffer *buf, *tmp;
	unsigned long flags;
	u32 i;

	/* Stop hardware */
	iowrite32(0, vcam->regs + VCAM_CTRL);
	vcam_hw_notify_ctrl();

	/* Cancel poll timer */
	cancel_delayed_work_sync(&vcam->poll_work);

	/* Return any remaining buffers to VB2 */
	spin_lock_irqsave(&vcam->buf_lock, flags);

	/* Buffers still in the ring */
	for (i = 0; i < VCAM_RING_SIZE; i++) {
		buf = vcam->ring_bufs[i];
		if (buf) {
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			vcam->ring_bufs[i] = NULL;
		}
	}

	/* Buffers still on the driver list */
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&vcam->buf_lock, flags);

	/* Tear down ring */
	vcam_hw_clear_buf_ring();
	kfree(vcam->ring_bufs);

	spin_lock_irqsave(&vcam->buf_lock, flags);
	kfree(vcam->ring);
	vcam->ring = NULL;
	spin_unlock_irqrestore(&vcam->buf_lock, flags);

	vcam->ring_bufs = NULL;

	pr_info(DRV_NAME ": streaming stopped\n");
}

static const struct vb2_ops vcam_vb2_ops = {
	.queue_setup     = vcam_queue_setup,
	.buf_prepare     = vcam_buf_prepare,
	.buf_queue       = vcam_buf_queue,
	.start_streaming = vcam_start_streaming,
	.stop_streaming  = vcam_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ---- V4L2 ioctl operations ---- */

static int vcam_querycap(struct file *file, void *fh,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VCAM-2000 Capture Ring", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vcam_ring", sizeof(cap->bus_info));
	return 0;
}

static int vcam_enum_fmt(struct file *file, void *fh,
			 struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_RGB24;
	strscpy(f->description, "RGB24", sizeof(f->description));
	return 0;
}

static int vcam_g_fmt(struct file *file, void *fh,
		      struct v4l2_format *f)
{
	struct vcam_device *vcam = video_drvdata(file);

	f->fmt.pix = vcam->fmt;
	return 0;
}

static void vcam_clamp_format(struct v4l2_pix_format *pix)
{
	pix->width  = clamp_t(u32, pix->width,
			      VCAM_HW_MIN_WIDTH, VCAM_HW_MAX_WIDTH);
	pix->height = clamp_t(u32, pix->height,
			      VCAM_HW_MIN_HEIGHT, VCAM_HW_MAX_HEIGHT);
	pix->width  = round_down(pix->width, VCAM_HW_STEP);
	pix->height = round_down(pix->height, VCAM_HW_STEP);

	pix->pixelformat  = V4L2_PIX_FMT_RGB24;
	pix->field         = V4L2_FIELD_NONE;
	pix->bytesperline  = pix->width * 3;
	pix->sizeimage     = pix->bytesperline * pix->height;
	pix->colorspace    = V4L2_COLORSPACE_SRGB;
}

static int vcam_try_fmt(struct file *file, void *fh,
			struct v4l2_format *f)
{
	vcam_clamp_format(&f->fmt.pix);
	return 0;
}

static int vcam_s_fmt(struct file *file, void *fh,
		      struct v4l2_format *f)
{
	struct vcam_device *vcam = video_drvdata(file);

	if (vb2_is_busy(&vcam->queue))
		return -EBUSY;

	vcam_clamp_format(&f->fmt.pix);
	vcam->fmt = f->fmt.pix;
	pr_info(DRV_NAME ": format set to %ux%u RGB24\n",
		vcam->fmt.width, vcam->fmt.height);
	return 0;
}

static int vcam_enum_framesizes(struct file *file, void *fh,
				struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_RGB24)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width   = VCAM_HW_MIN_WIDTH;
	fsize->stepwise.max_width   = VCAM_HW_MAX_WIDTH;
	fsize->stepwise.step_width  = VCAM_HW_STEP;
	fsize->stepwise.min_height  = VCAM_HW_MIN_HEIGHT;
	fsize->stepwise.max_height  = VCAM_HW_MAX_HEIGHT;
	fsize->stepwise.step_height = VCAM_HW_STEP;
	return 0;
}

static const struct v4l2_ioctl_ops vcam_ioctl_ops = {
	.vidioc_querycap         = vcam_querycap,
	.vidioc_enum_fmt_vid_cap = vcam_enum_fmt,
	.vidioc_g_fmt_vid_cap    = vcam_g_fmt,
	.vidioc_s_fmt_vid_cap    = vcam_s_fmt,
	.vidioc_try_fmt_vid_cap  = vcam_try_fmt,
	.vidioc_enum_framesizes  = vcam_enum_framesizes,
	/* VB2 helpers */
	.vidioc_reqbufs          = vb2_ioctl_reqbufs,
	.vidioc_querybuf         = vb2_ioctl_querybuf,
	.vidioc_qbuf             = vb2_ioctl_qbuf,
	.vidioc_dqbuf            = vb2_ioctl_dqbuf,
	.vidioc_streamon         = vb2_ioctl_streamon,
	.vidioc_streamoff        = vb2_ioctl_streamoff,
};

/* ---- File operations ---- */

static const struct v4l2_file_operations vcam_fops = {
	.owner   = THIS_MODULE,
	.open    = v4l2_fh_open,
	.release = vb2_fop_release,
	.mmap    = vb2_fop_mmap,
	.read    = vb2_fop_read,
	.poll    = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- Platform driver ---- */

static int vcam_probe(struct platform_device *pdev)
{
	struct vcam_device *vcam;
	struct vb2_queue *q;
	struct video_device *vdev;
	u32 chip_id;
	int ret;

	vcam = devm_kzalloc(&pdev->dev, sizeof(*vcam), GFP_KERNEL);
	if (!vcam)
		return -ENOMEM;

	vcam->pdev = pdev;
	mutex_init(&vcam->lock);
	spin_lock_init(&vcam->buf_lock);
	INIT_LIST_HEAD(&vcam->buf_list);
	INIT_DELAYED_WORK(&vcam->poll_work, vcam_poll_work);

	/* Map hardware register file */
	vcam->regs = vcam_hw_map_regs();
	if (!vcam->regs) {
		dev_err(&pdev->dev, "failed to map hardware registers\n");
		return -ENODEV;
	}

	/* Verify hardware */
	chip_id = ioread32(vcam->regs + VCAM_CHIP_ID);
	if (chip_id != VCAM_HW_CHIP_ID_VAL) {
		dev_err(&pdev->dev, "unexpected chip ID: 0x%08x\n", chip_id);
		ret = -ENODEV;
		goto err_unmap;
	}
	dev_info(&pdev->dev, "VCAM-2000 detected (rev %u)\n",
		 ioread32(vcam->regs + VCAM_CHIP_REV));

	/* Default format from hardware */
	vcam->fmt.width        = ioread32(vcam->regs + VCAM_FMT_WIDTH);
	vcam->fmt.height       = ioread32(vcam->regs + VCAM_FMT_HEIGHT);
	vcam->fmt.pixelformat  = V4L2_PIX_FMT_RGB24;
	vcam->fmt.field        = V4L2_FIELD_NONE;
	vcam->fmt.bytesperline = vcam->fmt.width * 3;
	vcam->fmt.sizeimage    = vcam->fmt.bytesperline * vcam->fmt.height;
	vcam->fmt.colorspace   = V4L2_COLORSPACE_SRGB;

	/* Register V4L2 device */
	ret = v4l2_device_register(&pdev->dev, &vcam->v4l2_dev);
	if (ret)
		goto err_unmap;

	/* Setup VB2 queue */
	q = &vcam->queue;
	q->type             = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes         = VB2_MMAP | VB2_READ;
	q->drv_priv         = vcam;
	q->buf_struct_size  = sizeof(struct vcam_buffer);
	q->ops              = &vcam_vb2_ops;
	q->mem_ops          = &vb2_vmalloc_memops;
	q->timestamp_flags  = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock             = &vcam->lock;
	q->dev              = &pdev->dev;

	ret = vb2_queue_init(q);
	if (ret)
		goto err_v4l2;

	/* Setup video device */
	vdev = &vcam->vdev;
	strscpy(vdev->name, DRV_NAME, sizeof(vdev->name));
	vdev->release    = video_device_release_empty;
	vdev->fops       = &vcam_fops;
	vdev->ioctl_ops  = &vcam_ioctl_ops;
	vdev->v4l2_dev   = &vcam->v4l2_dev;
	vdev->queue      = &vcam->queue;
	vdev->lock       = &vcam->lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING
			    | V4L2_CAP_READWRITE;
	vdev->vfl_type   = VFL_TYPE_VIDEO;

	video_set_drvdata(vdev, vcam);
	platform_set_drvdata(pdev, vcam);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;

	pr_info(DRV_NAME ": registered as /dev/video%d\n", vdev->num);
	return 0;

err_v4l2:
	v4l2_device_unregister(&vcam->v4l2_dev);
err_unmap:
	vcam_hw_unmap_regs();
	return ret;
}

static void vcam_remove(struct platform_device *pdev)
{
	struct vcam_device *vcam = platform_get_drvdata(pdev);

	video_unregister_device(&vcam->vdev);
	v4l2_device_unregister(&vcam->v4l2_dev);
	vcam_hw_unmap_regs();
	pr_info(DRV_NAME ": device removed\n");
}

static struct platform_driver vcam_driver = {
	.probe  = vcam_probe,
	.remove = vcam_remove,
	.driver = {
		.name = VCAM_HW_DEV_NAME,
	},
};

module_platform_driver(vcam_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("Part 4: V4L2 Capture with Descriptor Ring DMA (Polled)");
