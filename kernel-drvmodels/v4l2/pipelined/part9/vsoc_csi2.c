// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 CSI-2 Receiver Subdev -- Part 9: Media Links & Topology
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: SoC-internal subdev with 2 pads (sink + source)
 *
 * The CSI-2 receiver is a platform driver matching "vsoc_csi2".
 * It sits between the sensor and the bridge/DMA in the pipeline:
 *
 *   Sensor (source:0) --> CSI-2 (sink:0 | source:1) --> Bridge/Video
 *
 * Features:
 *   - 2 media pads: sink (from sensor) and source (to bridge)
 *   - Format passthrough: same format on both pads
 *   - MEDIA_ENT_F_VID_IF_BRIDGE entity function
 *   - s_stream writes CSI-2 CTRL register
 *   - Registered directly (not async) since it is SoC-internal
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_csi2.ko       (creates platform subdev)
 *   3. insmod vsoc_bridge.ko     (finds CSI-2 via platform device)
 *   4. insmod vsoc_sensor.ko     (async bound to bridge)
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/media-entity.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_csi2"

/* Pad indices */
#define CSI2_PAD_SINK		0
#define CSI2_PAD_SOURCE		1
#define CSI2_NUM_PADS		2

/* Supported media bus formats (same as sensor) */
static const u32 vsoc_csi2_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SRGGB8_1X8,
};

/* ====================================================================
 * Driver State
 * ==================================================================== */

struct vsoc_csi2 {
	struct platform_device	*pdev;
	struct v4l2_subdev	sd;
	struct media_pad	pads[CSI2_NUM_PADS];
	void __iomem		*regs;
	struct v4l2_mbus_framefmt fmt;
	bool			streaming;
};

static inline struct vsoc_csi2 *to_vsoc_csi2(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vsoc_csi2, sd);
}

/* ====================================================================
 * v4l2_subdev_video_ops
 * ==================================================================== */

/*
 * vsoc_csi2_s_stream — Part 9
 *
 * Step 1: If enabling — write format to CSI-2 hardware registers:
 *         WIDTH, HEIGHT, FMT code, LANES count
 * Step 2: If enabling — write ENABLE|STREAM to CTRL register
 * Step 3: If disabling — clear CTRL register
 * Step 4: Update streaming state
 *
 * First appearance in Part 9.
 */
static int vsoc_csi2_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vsoc_csi2 *csi2 = to_vsoc_csi2(sd);
	u32 ctrl;

	/* Step 1: If enabling — write format to CSI-2 hardware registers */
	if (enable) {
		iowrite32(csi2->fmt.width, csi2->regs + VSOC_CSI2_WIDTH);
		iowrite32(csi2->fmt.height, csi2->regs + VSOC_CSI2_HEIGHT);
		iowrite32(csi2->fmt.code, csi2->regs + VSOC_CSI2_FMT);
		iowrite32(4, csi2->regs + VSOC_CSI2_LANES); /* 4 lanes */

		/* Step 2: Write ENABLE|STREAM to CTRL register */
		ctrl = VSOC_CSI2_CTRL_ENABLE | VSOC_CSI2_CTRL_STREAM;
		iowrite32(ctrl, csi2->regs + VSOC_CSI2_CTRL);

		/* Step 4: Update streaming state */
		csi2->streaming = true;
		dev_info(&csi2->pdev->dev, "CSI-2 streaming ON (%ux%u)\n",
			 csi2->fmt.width, csi2->fmt.height);
	} else {
		/* Step 3: If disabling — clear CTRL register */
		iowrite32(0, csi2->regs + VSOC_CSI2_CTRL);

		/* Step 4: Update streaming state */
		csi2->streaming = false;
		dev_info(&csi2->pdev->dev, "CSI-2 streaming OFF\n");
	}

	return 0;
}

static const struct v4l2_subdev_video_ops vsoc_csi2_video_ops = {
	.s_stream = vsoc_csi2_s_stream,
};

/* ====================================================================
 * v4l2_subdev_pad_ops (format passthrough)
 * ==================================================================== */

static int vsoc_csi2_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(vsoc_csi2_mbus_codes))
		return -EINVAL;

	code->code = vsoc_csi2_mbus_codes[code->index];
	return 0;
}

static int vsoc_csi2_get_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct vsoc_csi2 *csi2 = to_vsoc_csi2(sd);

	/* Same format on both pads (passthrough) */
	format->format = csi2->fmt;
	return 0;
}

/*
 * vsoc_csi2_set_fmt — Part 9
 *
 * CSI-2 is a passthrough entity — same format on both pads.
 *
 * Step 1: Validate mbus code against supported list
 * Step 2: Clamp width [160, 3840] and height [120, 2160], align to 16
 * Step 3: Set field=NONE, colorspace=RAW
 * Step 4: If ACTIVE format — update csi2->fmt (applies to both pads)
 * Step 5: Return clamped format to caller
 *
 * First appearance in Part 9. Format passthrough: one format, both pads.
 */
static int vsoc_csi2_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct vsoc_csi2 *csi2 = to_vsoc_csi2(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	bool code_valid = false;
	unsigned int i;

	/* Step 1: Validate mbus code against supported list */
	for (i = 0; i < ARRAY_SIZE(vsoc_csi2_mbus_codes); i++) {
		if (fmt->code == vsoc_csi2_mbus_codes[i]) {
			code_valid = true;
			break;
		}
	}
	if (!code_valid)
		fmt->code = vsoc_csi2_mbus_codes[0];

	/* Step 2: Clamp dimensions and align to 16 */
	fmt->width  = clamp_t(u32, fmt->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	fmt->width  = rounddown(fmt->width, VSOC_SENSOR_STEP);
	fmt->height = rounddown(fmt->height, VSOC_SENSOR_STEP);

	/* Step 3: Set field=NONE, colorspace=RAW */
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;

	/* Step 4: If ACTIVE format — update csi2->fmt */
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		csi2->fmt = *fmt;

	/* Step 5: Return clamped format to caller */
	return 0;
}

static const struct v4l2_subdev_pad_ops vsoc_csi2_pad_ops = {
	.enum_mbus_code = vsoc_csi2_enum_mbus_code,
	.get_fmt        = vsoc_csi2_get_fmt,
	.set_fmt        = vsoc_csi2_set_fmt,
};

/* ====================================================================
 * Combined subdev ops
 * ==================================================================== */

static const struct v4l2_subdev_ops vsoc_csi2_subdev_ops = {
	.video = &vsoc_csi2_video_ops,
	.pad   = &vsoc_csi2_pad_ops,
};

/* ====================================================================
 * Platform Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_csi2_probe — Part 9: Media Links & CSI-2 Subdev
 *
 * CSI-2 is a SoC-internal block — uses platform driver (not I2C)
 * and direct registration (not async — it's always present).
 *
 * Step 1: Allocate driver state
 * Step 2: Map CSI-2 MMIO registers (vsoc_hw_map_csi2_regs)
 * Step 3: Set default format: 1920x1080, SRGGB10, V4L2_FIELD_NONE
 * Step 4: Initialize v4l2_subdev with ops (v4l2_subdev_init, not I2C variant)
 * Step 5: Set V4L2_SUBDEV_FL_HAS_DEVNODE for /dev/v4l-subdevN
 * Step 6: Initialize 2 media pads: sink (pad 0) + source (pad 1)
 * Step 7: Set entity function: MEDIA_ENT_F_VID_IF_BRIDGE
 * Step 8: Store via platform_set_drvdata for bridge discovery
 *
 * First appearance in Part 9. Bridge discovers us in .complete callback.
 */
static int vsoc_csi2_probe(struct platform_device *pdev)
{
	struct vsoc_csi2 *csi2;
	int ret;

	/* Step 1: Allocate driver state */
	csi2 = devm_kzalloc(&pdev->dev, sizeof(*csi2), GFP_KERNEL);
	if (!csi2)
		return -ENOMEM;

	csi2->pdev = pdev;

	/* Step 2: Map CSI-2 MMIO registers */
	csi2->regs = vsoc_hw_map_csi2_regs();
	if (!csi2->regs) {
		dev_err(&pdev->dev, "failed to map CSI-2 registers\n");
		return -ENOMEM;
	}

	/* Step 3: Set default format: 1920x1080, SRGGB10 */
	csi2->fmt.width      = VSOC_SENSOR_DEF_WIDTH;
	csi2->fmt.height     = VSOC_SENSOR_DEF_HEIGHT;
	csi2->fmt.code       = MEDIA_BUS_FMT_SRGGB10_1X10;
	csi2->fmt.field      = V4L2_FIELD_NONE;
	csi2->fmt.colorspace = V4L2_COLORSPACE_RAW;

	/* Step 4: Initialize v4l2_subdev with ops */
	v4l2_subdev_init(&csi2->sd, &vsoc_csi2_subdev_ops);
	/* Step 5: Set V4L2_SUBDEV_FL_HAS_DEVNODE for /dev/v4l-subdevN */
	csi2->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	csi2->sd.owner = THIS_MODULE;
	strscpy(csi2->sd.name, DRV_NAME, sizeof(csi2->sd.name));

	/* Step 6: Initialize 2 media pads: sink + source */
	csi2->pads[CSI2_PAD_SINK].flags   = MEDIA_PAD_FL_SINK;
	csi2->pads[CSI2_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&csi2->sd.entity,
				     CSI2_NUM_PADS, csi2->pads);
	if (ret) {
		dev_err(&pdev->dev, "media_entity_pads_init failed: %d\n",
			ret);
		goto err_unmap;
	}
	/* Step 7: Set entity function: MEDIA_ENT_F_VID_IF_BRIDGE */
	csi2->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	/* Step 8: Store via platform_set_drvdata for bridge discovery */
	platform_set_drvdata(pdev, csi2);

	dev_info(&pdev->dev,
		 "vsoc_csi2 probed (2 pads: sink + source)\n");
	return 0;

err_unmap:
	vsoc_hw_unmap_csi2_regs();
	return ret;
}

/*
 * vsoc_csi2_remove — Part 9
 *
 * Step 1: Clean up media entity
 * Step 2: Unmap CSI-2 MMIO registers
 */
static void vsoc_csi2_remove(struct platform_device *pdev)
{
	struct vsoc_csi2 *csi2 = platform_get_drvdata(pdev);

	/* Step 1: Clean up media entity */
	media_entity_cleanup(&csi2->sd.entity);
	/* Step 2: Unmap CSI-2 MMIO registers */
	vsoc_hw_unmap_csi2_regs();

	dev_info(&pdev->dev, "vsoc_csi2 removed\n");
}

/* ====================================================================
 * Platform Driver Registration
 * ==================================================================== */

static struct platform_driver vsoc_csi2_driver = {
	.probe  = vsoc_csi2_probe,
	.remove = vsoc_csi2_remove,
	.driver = {
		.name = VSOC_CSI2_DEV_NAME,
	},
};

module_platform_driver(vsoc_csi2_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 CSI-2 Receiver -- Part 9: Media Links & Topology");
MODULE_VERSION("1.0.0");
