// SPDX-License-Identifier: GPL-2.0
/*
 * Part 9: V4L2 Output Device with dma-buf Import
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Concepts introduced (over Part 8):
 *   - V4L2 OUTPUT device type (VIDEO_OUTPUT)
 *   - VFL_DIR_TX flag for output device registration
 *   - dma-buf import via V4L2 VB2_DMABUF memory mode
 *   - Reversed data flow — userspace pushes data to driver
 *   - DMA heap consumption pattern
 *
 * Carries forward:
 *   - VB2 buffer management, workqueue processing
 *   - Hardware register access (ioread32/iowrite32)
 *
 * NOT yet covered (see Part 10):
 *   - Full capture → export → import → output pipeline
 *
 * LOAD ORDER:
 *   sudo insmod ../hw/vcam_hw_platform.ko
 *   sudo insmod vout_dmabuf.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../hw/vcam_hw_interface.h"

#define DRV_NAME	"vout_dmabuf"

struct vout_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head       list;
};

struct vout_device {
	struct v4l2_device     v4l2_dev;
	struct video_device    vdev;
	struct vb2_queue       queue;
	struct v4l2_pix_format fmt;
	struct mutex           lock;
	struct platform_device *pdev;

	/* Hardware interface */
	void __iomem          *regs;

	struct list_head       buf_list;
	spinlock_t             buf_lock;

	struct work_struct     work;
	unsigned int           sequence;
};

/* ---- Buffer processing (work queue) ---- */

static void vout_process_buffer(struct work_struct *work)
{
	struct vout_device *vout = container_of(work, struct vout_device, work);
	struct vout_buffer *buf;
	unsigned long flags;
	void *vaddr;
	u32 marker;
	u8 *data;

	while (1) {
		spin_lock_irqsave(&vout->buf_lock, flags);
		if (list_empty(&vout->buf_list)) {
			spin_unlock_irqrestore(&vout->buf_lock, flags);
			break;
		}
		buf = list_first_entry(&vout->buf_list, struct vout_buffer,
				       list);
		list_del(&buf->list);
		spin_unlock_irqrestore(&vout->buf_lock, flags);

		vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
		if (vaddr) {
			data = vaddr;
			marker = *(u32 *)data;
			pr_info(DRV_NAME ": output frame %u, marker=0x%08x, "
				"pixel@4: R=%u G=%u B=%u\n",
				vout->sequence, marker,
				data[12], data[13], data[14]);
		} else {
			pr_info(DRV_NAME ": output frame %u (no vaddr)\n",
				vout->sequence);
		}

		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = vout->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
}

/* ---- VB2 operations ---- */

static int vout_queue_setup(struct vb2_queue *q,
			    unsigned int *num_buffers,
			    unsigned int *num_planes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct vout_device *vout = vb2_get_drv_priv(q);

	if (*num_buffers < 1)
		*num_buffers = 1;

	if (*num_planes)
		return sizes[0] < vout->fmt.sizeimage ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = vout->fmt.sizeimage;
	return 0;
}

static int vout_buf_prepare(struct vb2_buffer *vb)
{
	struct vout_device *vout = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < vout->fmt.sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, vout->fmt.sizeimage);
	return 0;
}

static void vout_buf_queue(struct vb2_buffer *vb)
{
	struct vout_device *vout = vb2_get_drv_priv(vb->vb2_queue);
	struct vout_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
					       struct vout_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vout->buf_lock, flags);
	list_add_tail(&buf->list, &vout->buf_list);
	spin_unlock_irqrestore(&vout->buf_lock, flags);

	schedule_work(&vout->work);
}

static int vout_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vout_device *vout = vb2_get_drv_priv(q);

	vout->sequence = 0;

	/* Program output format into hardware */
	iowrite32(vout->fmt.width,        vout->regs + VCAM_FMT_WIDTH);
	iowrite32(vout->fmt.height,       vout->regs + VCAM_FMT_HEIGHT);
	iowrite32(vout->fmt.bytesperline, vout->regs + VCAM_FMT_STRIDE);
	iowrite32(vout->fmt.sizeimage,    vout->regs + VCAM_FMT_FRAMESIZE);

	pr_info(DRV_NAME ": streaming started (%u buffers)\n", count);
	return 0;
}

static void vout_stop_streaming(struct vb2_queue *q)
{
	struct vout_device *vout = vb2_get_drv_priv(q);
	struct vout_buffer *buf, *tmp;
	unsigned long flags;

	cancel_work_sync(&vout->work);

	spin_lock_irqsave(&vout->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &vout->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vout->buf_lock, flags);

	pr_info(DRV_NAME ": streaming stopped\n");
}

static const struct vb2_ops vout_vb2_ops = {
	.queue_setup     = vout_queue_setup,
	.buf_prepare     = vout_buf_prepare,
	.buf_queue       = vout_buf_queue,
	.start_streaming = vout_start_streaming,
	.stop_streaming  = vout_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ---- V4L2 ioctl operations (OUTPUT variants) ---- */

static int vout_querycap(struct file *file, void *fh,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VCAM-2000 Display", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vout_dmabuf", sizeof(cap->bus_info));
	return 0;
}

static int vout_enum_fmt(struct file *file, void *fh,
			 struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_RGB24;
	strscpy(f->description, "RGB24", sizeof(f->description));
	return 0;
}

static int vout_g_fmt(struct file *file, void *fh,
		      struct v4l2_format *f)
{
	struct vout_device *vout = video_drvdata(file);

	f->fmt.pix = vout->fmt;
	return 0;
}

static void vout_clamp_format(struct v4l2_pix_format *pix)
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

static int vout_try_fmt(struct file *file, void *fh,
			struct v4l2_format *f)
{
	vout_clamp_format(&f->fmt.pix);
	return 0;
}

static int vout_s_fmt(struct file *file, void *fh,
		      struct v4l2_format *f)
{
	struct vout_device *vout = video_drvdata(file);

	if (vb2_is_busy(&vout->queue))
		return -EBUSY;

	vout_clamp_format(&f->fmt.pix);
	vout->fmt = f->fmt.pix;
	pr_info(DRV_NAME ": format set to %ux%u RGB24\n",
		vout->fmt.width, vout->fmt.height);
	return 0;
}

static const struct v4l2_ioctl_ops vout_ioctl_ops = {
	.vidioc_querycap          = vout_querycap,
	.vidioc_enum_fmt_vid_out  = vout_enum_fmt,
	.vidioc_g_fmt_vid_out     = vout_g_fmt,
	.vidioc_s_fmt_vid_out     = vout_s_fmt,
	.vidioc_try_fmt_vid_out   = vout_try_fmt,
	/* VB2 helpers */
	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,
	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,
	.vidioc_expbuf            = vb2_ioctl_expbuf,
};

/* ---- File operations ---- */

static const struct v4l2_file_operations vout_fops = {
	.owner   = THIS_MODULE,
	.open    = v4l2_fh_open,
	.release = vb2_fop_release,
	.mmap    = vb2_fop_mmap,
	.poll    = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- Platform driver ---- */

static int vout_probe(struct platform_device *pdev)
{
	struct vout_device *vout;
	struct vb2_queue *q;
	struct video_device *vdev;
	u32 chip_id;
	int ret;

	vout = devm_kzalloc(&pdev->dev, sizeof(*vout), GFP_KERNEL);
	if (!vout)
		return -ENOMEM;

	vout->pdev = pdev;
	mutex_init(&vout->lock);
	spin_lock_init(&vout->buf_lock);
	INIT_LIST_HEAD(&vout->buf_list);
	INIT_WORK(&vout->work, vout_process_buffer);

	/* Map hardware registers */
	vout->regs = vcam_hw_map_regs();
	if (!vout->regs) {
		dev_err(&pdev->dev, "failed to map hardware registers\n");
		return -ENODEV;
	}

	/* Verify hardware */
	chip_id = ioread32(vout->regs + VCAM_CHIP_ID);
	if (chip_id != VCAM_HW_CHIP_ID_VAL) {
		dev_err(&pdev->dev, "unexpected chip ID: 0x%08x\n", chip_id);
		vcam_hw_unmap_regs();
		return -ENODEV;
	}

	/* Default format from hardware */
	vout->fmt.width        = ioread32(vout->regs + VCAM_FMT_WIDTH);
	vout->fmt.height       = ioread32(vout->regs + VCAM_FMT_HEIGHT);
	vout->fmt.pixelformat  = V4L2_PIX_FMT_RGB24;
	vout->fmt.field        = V4L2_FIELD_NONE;
	vout->fmt.bytesperline = vout->fmt.width * 3;
	vout->fmt.sizeimage    = vout->fmt.bytesperline * vout->fmt.height;
	vout->fmt.colorspace   = V4L2_COLORSPACE_SRGB;

	/* Register V4L2 device */
	ret = v4l2_device_register(&pdev->dev, &vout->v4l2_dev);
	if (ret) {
		vcam_hw_unmap_regs();
		return ret;
	}

	/* Setup VB2 queue — OUTPUT type with DMABUF + MMAP */
	q = &vout->queue;
	q->type             = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	q->io_modes         = VB2_DMABUF | VB2_MMAP;
	q->drv_priv         = vout;
	q->buf_struct_size  = sizeof(struct vout_buffer);
	q->ops              = &vout_vb2_ops;
	q->mem_ops          = &vb2_vmalloc_memops;
	q->timestamp_flags  = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->lock             = &vout->lock;
	q->dev              = &pdev->dev;

	ret = vb2_queue_init(q);
	if (ret)
		goto err_v4l2;

	/* Setup video device */
	vdev = &vout->vdev;
	strscpy(vdev->name, DRV_NAME, sizeof(vdev->name));
	vdev->release    = video_device_release_empty;
	vdev->fops       = &vout_fops;
	vdev->ioctl_ops  = &vout_ioctl_ops;
	vdev->v4l2_dev   = &vout->v4l2_dev;
	vdev->queue      = &vout->queue;
	vdev->lock       = &vout->lock;
	vdev->device_caps = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING;
	vdev->vfl_type   = VFL_TYPE_VIDEO;
	vdev->vfl_dir    = VFL_DIR_TX;

	video_set_drvdata(vdev, vout);
	platform_set_drvdata(pdev, vout);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;

	pr_info(DRV_NAME ": registered as /dev/video%d\n", vdev->num);
	return 0;

err_v4l2:
	v4l2_device_unregister(&vout->v4l2_dev);
	vcam_hw_unmap_regs();
	return ret;
}

static void vout_remove(struct platform_device *pdev)
{
	struct vout_device *vout = platform_get_drvdata(pdev);

	video_unregister_device(&vout->vdev);
	v4l2_device_unregister(&vout->v4l2_dev);
	vcam_hw_unmap_regs();
	pr_info(DRV_NAME ": device removed\n");
}

static struct platform_driver vout_driver = {
	.probe  = vout_probe,
	.remove = vout_remove,
	.driver = {
		.name = VCAM_HW_OUT_DEV_NAME,
	},
};

module_platform_driver(vout_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("Part 9: V4L2 Output Device with dma-buf Import");
