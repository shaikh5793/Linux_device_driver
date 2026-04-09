// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 ISP Subdev -- Part 11: Multi-Subdev Pipeline
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: ISP (Image Signal Processor) subdev
 *
 * The ISP converts Bayer SRGGB10 data from the CSI-2 receiver into
 * RGB888 output for the DMA engine.  It has 2 pads:
 *   - Pad 0 (SINK):   accepts SRGGB10_1X10
 *   - Pad 1 (SOURCE): produces RGB888_1X24
 *
 * The ISP is modeled as MEDIA_ENT_F_PROC_VIDEO_ISP and sits in the
 * middle of the 4-entity pipeline:
 *
 *   Sensor -> CSI-2 -> ISP -> DMA (video node)
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

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/media-entity.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_isp"

/* Pad indices */
#define ISP_PAD_SINK		0
#define ISP_PAD_SOURCE		1
#define ISP_NUM_PADS		2

/* ====================================================================
 * Driver State
 * ==================================================================== */

struct vsoc_isp {
	struct v4l2_subdev	sd;
	struct media_pad	pads[ISP_NUM_PADS];
	void __iomem		*regs;
	struct v4l2_mbus_framefmt sink_fmt;
	struct v4l2_mbus_framefmt src_fmt;
	bool			streaming;
};

static inline struct vsoc_isp *to_vsoc_isp(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vsoc_isp, sd);
}

/* ====================================================================
 * v4l2_subdev_video_ops
 * ==================================================================== */

/*
 * vsoc_isp_s_stream — Part 11
 *
 * Step 1: If enabling — write input format code to ISP IN_FMT register
 * Step 2: If enabling — write output format code to ISP OUT_FMT register
 * Step 3: If enabling — write dimensions to WIDTH and HEIGHT registers
 * Step 4: If enabling — write ENABLE|STREAM to CTRL register
 * Step 5: If disabling — clear CTRL register
 * Step 6: Update streaming state
 *
 * First appearance in Part 11. ISP programs both input AND output format.
 */
static int vsoc_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vsoc_isp *isp = to_vsoc_isp(sd);
	u32 ctrl;

	if (enable) {
		/* Step 1: Write input format code to ISP IN_FMT register */
		iowrite32(isp->sink_fmt.code, isp->regs + VSOC_ISP_IN_FMT);
		/* Step 2: Write output format code to ISP OUT_FMT register */
		iowrite32(isp->src_fmt.code, isp->regs + VSOC_ISP_OUT_FMT);
		/* Step 3: Write dimensions to WIDTH and HEIGHT registers */
		iowrite32(isp->sink_fmt.width, isp->regs + VSOC_ISP_WIDTH);
		iowrite32(isp->sink_fmt.height, isp->regs + VSOC_ISP_HEIGHT);

		/* Step 4: Write ENABLE|STREAM to CTRL register */
		ctrl = VSOC_ISP_CTRL_ENABLE | VSOC_ISP_CTRL_STREAM;
		iowrite32(ctrl, isp->regs + VSOC_ISP_CTRL);

		/* Step 6: Update streaming state */
		isp->streaming = true;
		pr_info("vsoc_isp: ISP streaming ON (%ux%u Bayer->RGB)\n",
			isp->sink_fmt.width, isp->sink_fmt.height);
	} else {
		/* Step 5: If disabling — clear CTRL register */
		iowrite32(0, isp->regs + VSOC_ISP_CTRL);

		/* Step 6: Update streaming state */
		isp->streaming = false;
		pr_info("vsoc_isp: ISP streaming OFF\n");
	}

	return 0;
}

static const struct v4l2_subdev_video_ops vsoc_isp_video_ops = {
	.s_stream = vsoc_isp_s_stream,
};

/* ====================================================================
 * v4l2_subdev_pad_ops
 * ==================================================================== */

static int vsoc_isp_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	if (code->pad == ISP_PAD_SINK)
		code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	else
		code->code = MEDIA_BUS_FMT_RGB888_1X24;

	return 0;
}

static int vsoc_isp_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct vsoc_isp *isp = to_vsoc_isp(sd);

	if (format->pad == ISP_PAD_SINK)
		format->format = isp->sink_fmt;
	else
		format->format = isp->src_fmt;

	return 0;
}

/*
 * vsoc_isp_set_fmt — Part 11
 *
 * ISP is a converting entity — sink and source formats differ.
 *
 * Step 1: Clamp dimensions [160, 3840] x [120, 2160], align to 16
 * Step 2: If SINK pad:
 *         - Force code to SRGGB10 (only Bayer input accepted)
 *         - Set colorspace to RAW
 *         - If ACTIVE: propagate dimensions to source format
 * Step 3: If SOURCE pad:
 *         - Force code to RGB888 (ISP always outputs RGB)
 *         - Set colorspace to SRGB
 *         - Lock dimensions to match sink (ISP doesn't scale)
 * Step 4: If ACTIVE — update isp->sink_fmt or isp->src_fmt
 *
 * Key design: set SINK format first, source auto-propagates dimensions.
 * First appearance in Part 11.
 */
static int vsoc_isp_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct vsoc_isp *isp = to_vsoc_isp(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;

	/* Step 1: Clamp dimensions and align to 16 */
	fmt->width  = clamp_t(u32, fmt->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	fmt->width  = rounddown(fmt->width, VSOC_SENSOR_STEP);
	fmt->height = rounddown(fmt->height, VSOC_SENSOR_STEP);
	fmt->field  = V4L2_FIELD_NONE;

	/* Step 2: If SINK pad — force SRGGB10, propagate to source */
	if (format->pad == ISP_PAD_SINK) {
		fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
		fmt->colorspace = V4L2_COLORSPACE_RAW;

		/* Step 4: If ACTIVE — update isp->sink_fmt */
		if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
			isp->sink_fmt = *fmt;
			isp->src_fmt.width  = fmt->width;
			isp->src_fmt.height = fmt->height;
		}
	} else {
		/* Step 3: If SOURCE pad — force RGB888, lock to sink dims */
		fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
		fmt->colorspace = V4L2_COLORSPACE_SRGB;
		fmt->width  = isp->sink_fmt.width;
		fmt->height = isp->sink_fmt.height;

		/* Step 4: If ACTIVE — update isp->src_fmt */
		if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
			isp->src_fmt = *fmt;
	}

	return 0;
}

/*
 * vsoc_isp_link_validate — Part 11
 *
 * Custom validation: checks upstream source matches our sink format.
 *
 * Step 1: Compare width between upstream source and our sink
 * Step 2: Compare height
 * Step 3: Compare mbus code
 * Step 4: Return -EPIPE with detailed error log if any mismatch
 * Step 5: Return 0 if all match
 *
 * First appearance in Part 11. Unlike CSI-2 which uses
 * v4l2_subdev_link_validate_default, ISP has custom validation.
 */
static int vsoc_isp_link_validate(struct v4l2_subdev *sd,
				  struct media_link *link,
				  struct v4l2_subdev_format *source_fmt,
				  struct v4l2_subdev_format *sink_fmt)
{
	/* Step 1: Compare width between upstream source and our sink */
	/* Step 2: Compare height */
	/* Step 3: Compare mbus code */
	if (source_fmt->format.width != sink_fmt->format.width ||
	    source_fmt->format.height != sink_fmt->format.height ||
	    source_fmt->format.code != sink_fmt->format.code) {
		pr_err("vsoc_isp: link_validate failed: "
		       "source=%ux%u code=0x%04x sink=%ux%u code=0x%04x\n",
		       source_fmt->format.width, source_fmt->format.height,
		       source_fmt->format.code,
		       sink_fmt->format.width, sink_fmt->format.height,
		       sink_fmt->format.code);
		/* Step 4: Return -EPIPE on mismatch */
		return -EPIPE;
	}

	/* Step 5: Return 0 if all match */
	return 0;
}

static const struct v4l2_subdev_pad_ops vsoc_isp_pad_ops = {
	.enum_mbus_code = vsoc_isp_enum_mbus_code,
	.get_fmt        = vsoc_isp_get_fmt,
	.set_fmt        = vsoc_isp_set_fmt,
	.link_validate  = vsoc_isp_link_validate,
};

/* ====================================================================
 * init_state — default formats for subdev state
 * ==================================================================== */

/*
 * vsoc_isp_init_state — Part 11
 *
 * Called by framework when subdev state is first allocated.
 * Sets default format for both pads.
 *
 * Step 1: Set sink pad default: 1920x1080, SRGGB10, RAW
 * Step 2: Set source pad default: 1920x1080, RGB888, SRGB
 *
 * First appearance in Part 11.
 */
static int vsoc_isp_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;

	/* Step 1: Set sink pad default: 1920x1080, SRGGB10, RAW */
	fmt = v4l2_subdev_state_get_format(state, ISP_PAD_SINK);
	if (fmt) {
		fmt->width      = VSOC_SENSOR_DEF_WIDTH;
		fmt->height     = VSOC_SENSOR_DEF_HEIGHT;
		fmt->code       = MEDIA_BUS_FMT_SRGGB10_1X10;
		fmt->field      = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_RAW;
	}

	/* Step 2: Set source pad default: 1920x1080, RGB888, SRGB */
	fmt = v4l2_subdev_state_get_format(state, ISP_PAD_SOURCE);
	if (fmt) {
		fmt->width      = VSOC_SENSOR_DEF_WIDTH;
		fmt->height     = VSOC_SENSOR_DEF_HEIGHT;
		fmt->code       = MEDIA_BUS_FMT_RGB888_1X24;
		fmt->field      = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_SRGB;
	}

	return 0;
}

/* ====================================================================
 * Combined subdev ops
 * ==================================================================== */

static const struct v4l2_subdev_internal_ops vsoc_isp_internal_ops = {
	.init_state = vsoc_isp_init_state,
};

static const struct v4l2_subdev_ops vsoc_isp_subdev_ops = {
	.video = &vsoc_isp_video_ops,
	.pad   = &vsoc_isp_pad_ops,
};

/* ====================================================================
 * Platform Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_isp_probe — Part 11: Multi-Subdev Pipeline
 *
 * ISP is a format-converting entity: Bayer SRGGB10 in -> RGB888 out.
 * Like CSI-2, it's SoC-internal: platform driver + direct registration.
 *
 * Step 1: Allocate driver state
 * Step 2: Map ISP MMIO registers (vsoc_hw_map_isp_regs)
 * Step 3: Set default formats:
 *         sink: 1920x1080, SRGGB10, V4L2_COLORSPACE_RAW
 *         source: 1920x1080, RGB888, V4L2_COLORSPACE_SRGB
 * Step 4: Initialize v4l2_subdev with ops
 * Step 5: Set internal_ops with .init_state for default format state
 * Step 6: Set V4L2_SUBDEV_FL_HAS_DEVNODE
 * Step 7: Initialize 2 media pads: sink (pad 0) + source (pad 1)
 * Step 8: Set entity function: MEDIA_ENT_F_PROC_VIDEO_ISP
 * Step 9: Allocate subdev state via v4l2_subdev_init_finalize
 * Step 10: Store via platform_set_drvdata for bridge discovery
 *
 * First appearance in Part 11. Key difference from CSI-2:
 * ISP has SEPARATE sink and source formats (format conversion).
 */
static int vsoc_isp_probe(struct platform_device *pdev)
{
	struct vsoc_isp *isp;
	int ret;

	/* Step 1: Allocate driver state */
	isp = devm_kzalloc(&pdev->dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	/* Step 2: Map ISP MMIO registers */
	isp->regs = vsoc_hw_map_isp_regs();
	if (!isp->regs) {
		dev_err(&pdev->dev, "failed to map ISP registers\n");
		return -ENOMEM;
	}

	/* Step 3: Set default formats for sink and source */
	isp->sink_fmt.width      = VSOC_SENSOR_DEF_WIDTH;
	isp->sink_fmt.height     = VSOC_SENSOR_DEF_HEIGHT;
	isp->sink_fmt.code       = MEDIA_BUS_FMT_SRGGB10_1X10;
	isp->sink_fmt.field      = V4L2_FIELD_NONE;
	isp->sink_fmt.colorspace = V4L2_COLORSPACE_RAW;

	isp->src_fmt.width      = VSOC_SENSOR_DEF_WIDTH;
	isp->src_fmt.height     = VSOC_SENSOR_DEF_HEIGHT;
	isp->src_fmt.code       = MEDIA_BUS_FMT_RGB888_1X24;
	isp->src_fmt.field      = V4L2_FIELD_NONE;
	isp->src_fmt.colorspace = V4L2_COLORSPACE_SRGB;

	/* Step 4: Initialize v4l2_subdev with ops */
	v4l2_subdev_init(&isp->sd, &vsoc_isp_subdev_ops);
	/* Step 5: Set internal_ops with .init_state */
	isp->sd.internal_ops = &vsoc_isp_internal_ops;
	/* Step 6: Set V4L2_SUBDEV_FL_HAS_DEVNODE */
	isp->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	isp->sd.owner = THIS_MODULE;
	strscpy(isp->sd.name, DRV_NAME, sizeof(isp->sd.name));

	/* Step 7: Initialize 2 media pads: sink + source */
	isp->pads[ISP_PAD_SINK].flags   = MEDIA_PAD_FL_SINK;
	isp->pads[ISP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&isp->sd.entity,
				     ISP_NUM_PADS, isp->pads);
	if (ret) {
		dev_err(&pdev->dev, "media_entity_pads_init failed: %d\n",
			ret);
		goto err_unmap;
	}
	/* Step 8: Set entity function: MEDIA_ENT_F_PROC_VIDEO_ISP */
	isp->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;

	/* Step 10: Store via platform_set_drvdata for bridge discovery */
	platform_set_drvdata(pdev, isp);

	/* Step 9: Allocate subdev state via v4l2_subdev_init_finalize */
	ret = v4l2_subdev_init_finalize(&isp->sd);
	if (ret) {
		dev_err(&pdev->dev,
			"v4l2_subdev_init_finalize failed: %d\n", ret);
		goto err_entity;
	}

	dev_info(&pdev->dev,
		 "vsoc_isp probed (2 pads: SRGGB10 sink -> RGB888 source)\n");
	return 0;

err_entity:
	media_entity_cleanup(&isp->sd.entity);
err_unmap:
	vsoc_hw_unmap_isp_regs();
	return ret;
}

/*
 * vsoc_isp_remove — Part 11
 *
 * Step 1: Clean up subdev state (v4l2_subdev_cleanup)
 * Step 2: Clean up media entity
 * Step 3: Unmap ISP MMIO registers
 */
static void vsoc_isp_remove(struct platform_device *pdev)
{
	struct vsoc_isp *isp = platform_get_drvdata(pdev);

	/* Step 1: Clean up subdev state */
	v4l2_subdev_cleanup(&isp->sd);
	/* Step 2: Clean up media entity */
	media_entity_cleanup(&isp->sd.entity);
	/* Step 3: Unmap ISP MMIO registers */
	vsoc_hw_unmap_isp_regs();

	dev_info(&pdev->dev, "vsoc_isp removed\n");
}

/* ====================================================================
 * Platform Driver Registration
 * ==================================================================== */

static struct platform_driver vsoc_isp_driver = {
	.probe  = vsoc_isp_probe,
	.remove = vsoc_isp_remove,
	.driver = {
		.name = VSOC_ISP_DEV_NAME,
	},
};

module_platform_driver(vsoc_isp_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 ISP Subdev -- Part 11: Multi-Subdev Pipeline");
MODULE_VERSION("1.0.0");
