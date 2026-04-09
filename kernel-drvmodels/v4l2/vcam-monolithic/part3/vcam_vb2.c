// SPDX-License-Identifier: GPL-2.0
/*
 * Part 3: V4L2 Capture Device with VB2 Streaming (Timer-Based)
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Concepts introduced (over Part 2):
 *   - VB2 (videobuf2) buffer management and streaming
 *   - The five core VB2 callbacks: queue_setup, buf_prepare,
 *     buf_queue, start_streaming, stop_streaming
 *   - Timer-driven frame generation (~30fps via delayed work)
 *   - MMAP buffer mapping for userspace access
 *   - Buffer lifecycle: REQBUFS → QBUF → STREAMON → DQBUF
 *
 * Carries forward from Part 2:
 *   - Hardware register access (ioread32/iowrite32)
 *   - V4L2 device/video_device registration
 *   - Format negotiation ioctls
 *
 * NOT yet covered (see Part 4):
 *   - Hardware descriptor ring for DMA-style buffer delivery
 *   - Interrupt-driven frame completion
 *
 * Design note:
 *   This part uses a simple delayed_work timer to simulate frame
 *   delivery. Each timer tick picks the next buffer from the driver
 *   list, fills it via hardware, and returns it to VB2. This is the
 *   simplest streaming model — real hardware would use DMA and
 *   interrupts (Parts 4 and 5).
 *
 * LOAD ORDER:
 *   sudo insmod ../hw/vcam_hw_platform.ko
 *   sudo insmod vcam_vb2.ko
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

#define DRV_NAME	"vcam_vb2"

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

	/* Driver-internal buffer list */
	struct list_head       buf_list;
	spinlock_t             buf_lock;

	/* Timer-based frame delivery */
	struct delayed_work    frame_work;
	bool                   streaming;
	unsigned int           sequence;
};

/* ---- Timer-based frame delivery ---- */

/*
 * vcam_fill_buffer -- Ask hardware to generate a frame into the buffer
 *
 * In a real driver, the DMA engine writes directly to the buffer.
 * Here, we write format parameters to hardware and let the hw
 * platform fill the buffer contents at the virtual address.
 */
static void vcam_fill_buffer(struct vcam_device *vcam,
			     struct vcam_buffer *buf)
{
	void *vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

	/* Hardware uses the configured format to generate pixel data */
	iowrite32(vcam->fmt.width,        vcam->regs + VCAM_FMT_WIDTH);
	iowrite32(vcam->fmt.height,       vcam->regs + VCAM_FMT_HEIGHT);
	iowrite32(vcam->fmt.pixelformat,  vcam->regs + VCAM_FMT_PIXFMT);
	iowrite32(vcam->fmt.bytesperline, vcam->regs + VCAM_FMT_STRIDE);
	iowrite32(vcam->fmt.sizeimage,    vcam->regs + VCAM_FMT_FRAMESIZE);

	/*
	 * In a real camera, the DMA engine fills the buffer at a
	 * physical address. Our virtual hardware fills it at a
	 * virtual address for demonstration purposes.
	 */
	memset(vaddr, (vcam->sequence * 37) & 0xff, vcam->fmt.sizeimage);
	/* Write frame counter in the first 4 bytes */
	*(u32 *)vaddr = vcam->sequence;
}

/*
 * vcam_frame_work -- Periodic work function simulating frame capture
 *
 * Called every ~33ms (30fps).  Picks the oldest buffer from the
 * driver list, fills it, timestamps it, and returns it to VB2.
 * Then re-arms the timer for the next frame.
 *
 * This is the simplest possible streaming model:
 *   1. Take a buffer from the driver queue
 *   2. Fill it with frame data
 *   3. Return it to VB2 via vb2_buffer_done()
 *   4. Schedule the next frame
 */
static void vcam_frame_work(struct work_struct *work)
{
	struct vcam_device *vcam =
		container_of(work, struct vcam_device, frame_work.work);
	struct vcam_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&vcam->buf_lock, flags);
	if (!vcam->streaming || list_empty(&vcam->buf_list)) {
		spin_unlock_irqrestore(&vcam->buf_lock, flags);
		return;
	}
	buf = list_first_entry(&vcam->buf_list, struct vcam_buffer, list);
	list_del(&buf->list);
	spin_unlock_irqrestore(&vcam->buf_lock, flags);

	/* Fill the buffer with frame data */
	vcam_fill_buffer(vcam, buf);

	/* Timestamp and return to VB2 */
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = vcam->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	/* Schedule next frame at ~30fps */
	if (vcam->streaming)
		schedule_delayed_work(&vcam->frame_work,
				      msecs_to_jiffies(VCAM_HW_FRAME_MS));
}

/* ---- VB2 operations ---- */

/*
 * vcam_queue_setup -- Negotiate buffer count and sizes with VB2
 *
 * Called by VIDIOC_REQBUFS.  The driver tells VB2 how many planes
 * each buffer has and how large each plane must be.
 *
 * For a simple RGB24 camera: 1 plane, size = width * height * 3.
 */
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

/*
 * vcam_buf_prepare -- Validate and prepare a buffer before queuing
 *
 * Called for each buffer before it enters the driver's queue.
 * Sets the payload size so VB2 and userspace know how much data
 * to expect.
 */
static int vcam_buf_prepare(struct vb2_buffer *vb)
{
	struct vcam_device *vcam = vb2_get_drv_priv(vb->vb2_queue);

	vb2_set_plane_payload(vb, 0, vcam->fmt.sizeimage);
	return 0;
}

/*
 * vcam_buf_queue -- VB2 hands a buffer to the driver for filling
 *
 * Called by VIDIOC_QBUF.  The driver adds the buffer to its
 * internal list.  The timer work function will pick it up and
 * fill it with frame data.
 */
static void vcam_buf_queue(struct vb2_buffer *vb)
{
	struct vcam_device *vcam = vb2_get_drv_priv(vb->vb2_queue);
	struct vcam_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
					       struct vcam_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vcam->buf_lock, flags);
	list_add_tail(&buf->list, &vcam->buf_list);
	spin_unlock_irqrestore(&vcam->buf_lock, flags);
}

/*
 * vcam_start_streaming -- Begin frame capture
 *
 * Called by VIDIOC_STREAMON after VB2 has queued the minimum
 * number of buffers.  Enables the hardware and starts the
 * frame timer.
 */
static int vcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vcam_device *vcam = vb2_get_drv_priv(q);

	vcam->sequence = 0;
	vcam->streaming = true;

	/* Enable hardware */
	iowrite32(VCAM_CTRL_ENABLE | VCAM_CTRL_STREAM_ON,
		  vcam->regs + VCAM_CTRL);
	vcam_hw_notify_ctrl();

	/* Start frame timer */
	schedule_delayed_work(&vcam->frame_work,
			      msecs_to_jiffies(VCAM_HW_FRAME_MS));

	pr_info(DRV_NAME ": streaming started (%u buffers)\n", count);
	return 0;
}

/*
 * vcam_stop_streaming -- Stop frame capture and return all buffers
 *
 * Called by VIDIOC_STREAMOFF.  Must return ALL buffers to VB2 —
 * this is a hard requirement.  Buffers not returned will leak.
 */
static void vcam_stop_streaming(struct vb2_queue *q)
{
	struct vcam_device *vcam = vb2_get_drv_priv(q);
	struct vcam_buffer *buf, *tmp;
	unsigned long flags;

	/* Stop hardware */
	vcam->streaming = false;
	iowrite32(0, vcam->regs + VCAM_CTRL);
	vcam_hw_notify_ctrl();

	/* Cancel pending timer */
	cancel_delayed_work_sync(&vcam->frame_work);

	/* Return all buffers to VB2 */
	spin_lock_irqsave(&vcam->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vcam->buf_lock, flags);

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
	strscpy(cap->card, "VCAM-2000 Capture VB2", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vcam_vb2", sizeof(cap->bus_info));
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
	INIT_DELAYED_WORK(&vcam->frame_work, vcam_frame_work);

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
MODULE_DESCRIPTION("Part 3: V4L2 Capture with VB2 Streaming (Timer-Based)");
