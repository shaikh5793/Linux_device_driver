// SPDX-License-Identifier: GPL-2.0
/*
 * Part 2: Minimal V4L2 Capture Device — Format Negotiation
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Concepts introduced:
 *   - V4L2 device registration (v4l2_device, video_device)
 *   - Format negotiation ioctls (ENUM_FMT, G/S/TRY_FMT)
 *   - Hardware register access via ioread32/iowrite32
 *   - Reading CHIP_ID to verify hardware presence
 *   - Writing format parameters to hardware registers
 *
 * NOT yet covered (see Part 3):
 *   - VB2 streaming, buffer management, interrupts
 *
 * LOAD ORDER:
 *   sudo insmod ../hw/vcam_hw_platform.ko   ← creates "vcam_hw" platform device
 *   sudo insmod vcam.ko                     ← bus matches, probe() called automatically
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>

#include "../hw/vcam_hw_interface.h"

#define DRV_NAME "vcam"

struct vcam_device {
	struct v4l2_device  v4l2_dev;
	struct video_device vdev;
	struct v4l2_pix_format fmt;
	struct platform_device *pdev;

	/* Hardware interface */
	void __iomem *regs;
};

/* ---- V4L2 ioctl operations ---- */

static int vcam_querycap(struct file *file, void *fh,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VCAM-2000 Capture", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vcam", sizeof(cap->bus_info));
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

	vcam_clamp_format(&f->fmt.pix);
	vcam->fmt = f->fmt.pix;

	/* Program format into hardware registers */
	iowrite32(vcam->fmt.width,        vcam->regs + VCAM_FMT_WIDTH);
	iowrite32(vcam->fmt.height,       vcam->regs + VCAM_FMT_HEIGHT);
	iowrite32(vcam->fmt.pixelformat,  vcam->regs + VCAM_FMT_PIXFMT);
	iowrite32(vcam->fmt.bytesperline, vcam->regs + VCAM_FMT_STRIDE);
	iowrite32(vcam->fmt.sizeimage,    vcam->regs + VCAM_FMT_FRAMESIZE);
	/*
	 * Program format into hardware registers.  In a real camera
	 * driver, this configures the ISP's input format, DMA stride,
	 * and output size.  The hardware uses these values to know
	 * how to interpret and transfer frame data.
	 */

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
};

/* ---- File operations ---- */

static const struct v4l2_file_operations vcam_fops = {
	.owner   = THIS_MODULE,
	.open    = v4l2_fh_open,
	.release = v4l2_fh_release,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- Platform driver ---- */

static int vcam_probe(struct platform_device *pdev)
{
	struct vcam_device *vcam;
	struct video_device *vdev;
	u32 chip_id;
	int ret;

	vcam = devm_kzalloc(&pdev->dev, sizeof(*vcam), GFP_KERNEL);
	if (!vcam)
		return -ENOMEM;

	vcam->pdev = pdev;

	/* Map hardware register file */
	vcam->regs = vcam_hw_map_regs();
	if (!vcam->regs) {
		dev_err(&pdev->dev, "failed to map hardware registers\n");
		return -ENODEV;
	}

	/* Verify hardware presence by reading chip ID */
	chip_id = ioread32(vcam->regs + VCAM_CHIP_ID);
	if (chip_id != VCAM_HW_CHIP_ID_VAL) {
		dev_err(&pdev->dev, "unexpected chip ID: 0x%08x\n", chip_id);
		vcam_hw_unmap_regs();
		return -ENODEV;
	}
	/*
	 * CHIP_ID verification: every hardware driver reads a known
	 * register at probe time to confirm the expected device is
	 * present.  This catches wrong module loads, hardware failures,
	 * and mismatched device tree configurations.
	 */
	dev_info(&pdev->dev, "VCAM-2000 detected (chip ID 0x%08x, rev %u)\n",
		 chip_id, ioread32(vcam->regs + VCAM_CHIP_REV));

	/* Read default format from hardware */
	vcam->fmt.width        = ioread32(vcam->regs + VCAM_FMT_WIDTH);
	vcam->fmt.height       = ioread32(vcam->regs + VCAM_FMT_HEIGHT);
	vcam->fmt.pixelformat  = V4L2_PIX_FMT_RGB24;
	vcam->fmt.field        = V4L2_FIELD_NONE;
	vcam->fmt.bytesperline = vcam->fmt.width * 3;
	vcam->fmt.sizeimage    = vcam->fmt.bytesperline * vcam->fmt.height;
	vcam->fmt.colorspace   = V4L2_COLORSPACE_SRGB;

	/* Register V4L2 device */
	ret = v4l2_device_register(&pdev->dev, &vcam->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "v4l2_device_register failed: %d\n", ret);
		vcam_hw_unmap_regs();
		return ret;
	}

	/* Setup video device */
	vdev = &vcam->vdev;
	strscpy(vdev->name, DRV_NAME, sizeof(vdev->name));
	vdev->release    = video_device_release_empty;
	vdev->fops       = &vcam_fops;
	vdev->ioctl_ops  = &vcam_ioctl_ops;
	vdev->v4l2_dev   = &vcam->v4l2_dev;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE;
	vdev->vfl_type   = VFL_TYPE_VIDEO;

	video_set_drvdata(vdev, vcam);
	platform_set_drvdata(pdev, vcam);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&pdev->dev, "video_register_device failed: %d\n", ret);
		v4l2_device_unregister(&vcam->v4l2_dev);
		vcam_hw_unmap_regs();
		return ret;
	}

	pr_info(DRV_NAME ": registered as /dev/video%d\n", vdev->num);
	return 0;
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
MODULE_DESCRIPTION("Part 2: Minimal V4L2 Capture Device with VCAM-2000");
