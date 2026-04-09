// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor — Part 5: Subdev Controls
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: v4l2_ctrl_handler with sensor image controls
 *
 * Builds on Part 4 by adding:
 *   - v4l2_ctrl_handler with 5 controls:
 *     - V4L2_CID_EXPOSURE (0-65535, default 1000)
 *     - V4L2_CID_ANALOGUE_GAIN (0-255, default 64)
 *     - V4L2_CID_DIGITAL_GAIN (0-255, default 64)
 *     - V4L2_CID_HFLIP (boolean, default 0)
 *     - V4L2_CID_VFLIP (boolean, default 0)
 *   - s_ctrl callback writes values to sensor I2C registers
 *   - v4l2_ctrl_handler_setup() applies defaults at probe
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_sensor.ko
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_sensor"

#define VSOC_SENSOR_PAD_SOURCE	0
#define VSOC_SENSOR_NUM_PADS	1
#define VSOC_SENSOR_NUM_CTRLS	5

/* Supported media bus formats */
static const u32 vsoc_sensor_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_RGB888_1X24,
};

struct vsoc_sensor {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct v4l2_device v4l2_dev;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	bool streaming;
};

static inline struct vsoc_sensor *to_vsoc_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vsoc_sensor, sd);
}

/* ====================================================================
 * I2C Register Access
 * ==================================================================== */

static int vsoc_sensor_read_reg(struct i2c_client *client, u16 reg)
{
	int ret = i2c_smbus_read_word_data(client, reg);

	if (ret < 0)
		dev_err(&client->dev, "read reg 0x%02x failed: %d\n",
			reg, ret);
	return ret;
}

static int vsoc_sensor_write_reg(struct i2c_client *client, u16 reg, u16 val)
{
	int ret = i2c_smbus_write_word_data(client, reg, val);

	if (ret < 0)
		dev_err(&client->dev, "write reg 0x%02x failed: %d\n",
			reg, ret);
	return ret;
}

/* ====================================================================
 * v4l2_ctrl_ops — NEW in Part 5
 * ==================================================================== */

/*
 * vsoc_sensor_s_ctrl -- Part 5: Control Write
 *
 * Step 1: Recover driver state from ctrl->handler via container_of
 * Step 2: Switch on ctrl->id:
 *         - EXPOSURE:      write to VSOC_SENSOR_EXPOSURE register
 *         - ANALOGUE_GAIN: write to VSOC_SENSOR_GAIN register
 *         - DIGITAL_GAIN:  write to VSOC_SENSOR_DGAIN register
 *         - HFLIP:         write to VSOC_SENSOR_HFLIP register
 *         - VFLIP:         write to VSOC_SENSOR_VFLIP register
 *
 * First appearance in Part 5.
 */
static int vsoc_sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	/* Step 1: Recover driver state from ctrl->handler via container_of */
	struct vsoc_sensor *sensor =
		container_of(ctrl->handler, struct vsoc_sensor, ctrl_handler);
	struct i2c_client *client = sensor->client;
	int ret = 0;

	/* Step 2: Switch on ctrl->id and write to corresponding sensor register */
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = vsoc_sensor_write_reg(client, VSOC_SENSOR_EXPOSURE,
					    ctrl->val);
		dev_dbg(&client->dev, "exposure set to %d\n", ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = vsoc_sensor_write_reg(client, VSOC_SENSOR_GAIN,
					    ctrl->val);
		dev_dbg(&client->dev, "analogue gain set to %d\n", ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = vsoc_sensor_write_reg(client, VSOC_SENSOR_DGAIN,
					    ctrl->val);
		dev_dbg(&client->dev, "digital gain set to %d\n", ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = vsoc_sensor_write_reg(client, VSOC_SENSOR_HFLIP,
					    ctrl->val);
		dev_dbg(&client->dev, "hflip set to %d\n", ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		ret = vsoc_sensor_write_reg(client, VSOC_SENSOR_VFLIP,
					    ctrl->val);
		dev_dbg(&client->dev, "vflip set to %d\n", ctrl->val);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const struct v4l2_ctrl_ops vsoc_sensor_ctrl_ops = {
	.s_ctrl = vsoc_sensor_s_ctrl,
};

/* ====================================================================
 * Format helpers
 * ==================================================================== */

static bool vsoc_sensor_is_valid_mbus_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vsoc_sensor_mbus_codes); i++) {
		if (vsoc_sensor_mbus_codes[i] == code)
			return true;
	}
	return false;
}

static void vsoc_sensor_clamp_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = clamp_t(u32, fmt->width,
			     VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	fmt->width = rounddown(fmt->width, VSOC_SENSOR_STEP);

	fmt->height = clamp_t(u32, fmt->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	fmt->height = rounddown(fmt->height, VSOC_SENSOR_STEP);

	if (!vsoc_sensor_is_valid_mbus_code(fmt->code))
		fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;

	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

/* ====================================================================
 * v4l2_subdev_pad_ops — from Part 4
 * ==================================================================== */

static int vsoc_sensor_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(vsoc_sensor_mbus_codes))
		return -EINVAL;

	code->code = vsoc_sensor_mbus_codes[code->index];
	return 0;
}

static int vsoc_sensor_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0)
		return -EINVAL;

	if (!vsoc_sensor_is_valid_mbus_code(fse->code))
		return -EINVAL;

	fse->min_width = VSOC_SENSOR_MIN_WIDTH;
	fse->max_width = VSOC_SENSOR_MAX_WIDTH;
	fse->min_height = VSOC_SENSOR_MIN_HEIGHT;
	fse->max_height = VSOC_SENSOR_MAX_HEIGHT;

	return 0;
}

static int vsoc_sensor_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, format->pad);
	if (!fmt)
		return -EINVAL;

	format->format = *fmt;
	return 0;
}

static int vsoc_sensor_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	struct v4l2_mbus_framefmt *fmt;

	vsoc_sensor_clamp_format(&format->format);

	fmt = v4l2_subdev_state_get_format(state, format->pad);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		vsoc_sensor_write_reg(sensor->client, VSOC_SENSOR_WIDTH,
				      fmt->width);
		vsoc_sensor_write_reg(sensor->client, VSOC_SENSOR_HEIGHT,
				      fmt->height);
		vsoc_sensor_write_reg(sensor->client, VSOC_SENSOR_FMT,
				      fmt->code & 0xFFFF);
		dev_info(&sensor->client->dev,
			 "format set: %ux%u code=0x%04x\n",
			 fmt->width, fmt->height, fmt->code);
	}

	return 0;
}

static int vsoc_sensor_init_state(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, VSOC_SENSOR_PAD_SOURCE);
	if (fmt) {
		fmt->width = VSOC_SENSOR_DEF_WIDTH;
		fmt->height = VSOC_SENSOR_DEF_HEIGHT;
		fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_RAW;
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops vsoc_sensor_pad_ops = {
	.enum_mbus_code  = vsoc_sensor_enum_mbus_code,
	.enum_frame_size = vsoc_sensor_enum_frame_size,
	.get_fmt         = vsoc_sensor_get_fmt,
	.set_fmt         = vsoc_sensor_set_fmt,
};

/* ====================================================================
 * v4l2_subdev_video_ops — from Part 3
 * ==================================================================== */

static int vsoc_sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	u16 ctrl;
	int ret;

	if (enable) {
		ctrl = VSOC_SENSOR_CTRL_ENABLE | VSOC_SENSOR_CTRL_STREAM;
		ret = vsoc_sensor_write_reg(sensor->client,
					    VSOC_SENSOR_CTRL, ctrl);
		if (ret)
			return ret;

		sensor->streaming = true;
		dev_info(&sensor->client->dev, "sensor streaming ON\n");
	} else {
		ret = vsoc_sensor_write_reg(sensor->client,
					    VSOC_SENSOR_CTRL, 0);
		if (ret)
			return ret;

		sensor->streaming = false;
		dev_info(&sensor->client->dev, "sensor streaming OFF\n");
	}

	return 0;
}

static const struct v4l2_subdev_video_ops vsoc_sensor_video_ops = {
	.s_stream = vsoc_sensor_s_stream,
};

/* ====================================================================
 * v4l2_subdev_core_ops — from Part 3
 * ==================================================================== */

static int vsoc_sensor_log_status(struct v4l2_subdev *sd)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	int chip_id, status;

	chip_id = vsoc_sensor_read_reg(sensor->client, VSOC_SENSOR_CHIP_ID);
	status  = vsoc_sensor_read_reg(sensor->client, VSOC_SENSOR_STATUS);

	dev_info(&sensor->client->dev,
		 "=== Sensor Status ===\n"
		 "  Chip ID:   0x%04x\n"
		 "  Status:    0x%04x (ready=%d, streaming=%d)\n"
		 "  Streaming: %s\n",
		 chip_id, status,
		 !!(status & VSOC_SENSOR_STATUS_READY),
		 !!(status & VSOC_SENSOR_STATUS_STREAMING),
		 sensor->streaming ? "yes" : "no");

	return 0;
}

static const struct v4l2_subdev_core_ops vsoc_sensor_core_ops = {
	.log_status = vsoc_sensor_log_status,
};

/* Combine all ops categories */
static const struct v4l2_subdev_ops vsoc_sensor_subdev_ops = {
	.core  = &vsoc_sensor_core_ops,
	.video = &vsoc_sensor_video_ops,
	.pad   = &vsoc_sensor_pad_ops,
};

static const struct v4l2_subdev_internal_ops vsoc_sensor_internal_ops = {
	.init_state = vsoc_sensor_init_state,
};

/* ====================================================================
 * I2C Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_sensor_init_controls -- Part 5: Control Initialization
 *
 * Step 1: Initialize ctrl_handler for 5 controls
 * Step 2: Create standard controls with v4l2_ctrl_new_std (deferred error)
 *         - EXPOSURE     [0 .. 65535, step 1, default 1000]
 *         - ANALOGUE_GAIN [0 .. 255, step 1, default 64]
 *         - DIGITAL_GAIN  [0 .. 255, step 1, default 64]
 *         - HFLIP        [0 .. 1]
 *         - VFLIP        [0 .. 1]
 * Step 3: Check hdl->error once (deferred error pattern)
 * Step 4: Link ctrl_handler to subdev (sd->ctrl_handler = &handler)
 *
 * First appearance in Part 5.
 */
static int vsoc_sensor_init_controls(struct vsoc_sensor *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	int ret;

	/* Step 1: Initialize ctrl_handler for 5 controls */
	ret = v4l2_ctrl_handler_init(hdl, VSOC_SENSOR_NUM_CTRLS);
	if (ret)
		return ret;

	/* Step 2: Create standard controls with v4l2_ctrl_new_std (deferred error) */
	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_EXPOSURE,
			  0, VSOC_SENSOR_MAX_EXPOSURE, 1,
			  VSOC_SENSOR_DEF_EXPOSURE);

	/* Analogue gain: 0-255, step 1, default 64 */
	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN,
			  0, VSOC_SENSOR_MAX_GAIN, 1,
			  VSOC_SENSOR_DEF_GAIN);

	/* Digital gain: 0-255, step 1, default 64 */
	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN,
			  0, VSOC_SENSOR_MAX_GAIN, 1,
			  VSOC_SENSOR_DEF_GAIN);

	/* Horizontal flip: boolean, default 0 */
	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_HFLIP,
			  0, 1, 1, 0);

	/* Vertical flip: boolean, default 0 */
	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_VFLIP,
			  0, 1, 1, 0);

	/* Step 3: Check hdl->error once (deferred error pattern) */
	if (hdl->error) {
		v4l2_ctrl_handler_free(hdl);
		return hdl->error;
	}

	/* Step 4: Link ctrl_handler to subdev */
	sensor->sd.ctrl_handler = hdl;
	return 0;
}

/*
 * vsoc_sensor_probe -- Part 5: Subdev Controls
 *
 * Step 1:  Allocate and initialize driver state
 * Step 2:  Verify hardware -- read chip ID via I2C
 * Step 3:  Register temporary v4l2_device
 * Step 4:  Initialize v4l2_subdev with all ops
 * Step 5:  Set internal_ops (.init_state)
 * Step 6:  Initialize media entity with 1 source pad
 * Step 7:  [NEW] Initialize V4L2 controls (5 controls via init_controls helper)
 * Step 8:  Allocate subdev state via v4l2_subdev_init_finalize
 * Step 9:  Write default format to sensor registers
 * Step 10: [NEW] Apply default control values to hardware (v4l2_ctrl_handler_setup)
 * Step 11: Register subdev with v4l2_device
 * Step 12: Create /dev/v4l-subdevN
 *
 * Changes from Part 4:
 *   + Added control handler initialization (Step 7)
 *   + Added v4l2_ctrl_handler_setup to sync defaults to HW (Step 10)
 */
static int vsoc_sensor_probe(struct i2c_client *client)
{
	struct vsoc_sensor *sensor;
	int chip_id;
	int ret;

	/* Step 1: Allocate and initialize driver state */
	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;

	/* Step 2: Verify hardware -- read chip ID via I2C */
	chip_id = vsoc_sensor_read_reg(client, VSOC_SENSOR_CHIP_ID);
	if (chip_id < 0)
		return chip_id;
	if (chip_id != VSOC_SENSOR_CHIP_ID_VAL) {
		dev_err(&client->dev, "unexpected chip ID: 0x%04x\n", chip_id);
		return -ENODEV;
	}

	dev_info(&client->dev, "VSOC-3000 sensor detected (ID: 0x%04x)\n",
		 chip_id);

	/* Step 3: Register temporary v4l2_device for standalone operation */
	ret = v4l2_device_register(&client->dev, &sensor->v4l2_dev);
	if (ret)
		return ret;

	/* Step 4: Initialize v4l2_subdev with all ops */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vsoc_sensor_subdev_ops);
	/* Step 5: Set internal_ops (.init_state) */
	sensor->sd.internal_ops = &vsoc_sensor_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sensor->sd.name, DRV_NAME, sizeof(sensor->sd.name));

	/* Step 6: Initialize media entity with 1 source pad */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity,
				     VSOC_SENSOR_NUM_PADS, &sensor->pad);
	if (ret)
		goto err_v4l2_dev;

	/* Step 7: Initialize V4L2 controls (5 controls via init_controls helper) */
	ret = vsoc_sensor_init_controls(sensor);
	if (ret) {
		dev_err(&client->dev, "failed to init controls: %d\n", ret);
		goto err_entity;
	}

	/* Step 8: Allocate subdev state via v4l2_subdev_init_finalize */
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_ctrl;

	/* Step 9: Write default format to sensor registers */
	vsoc_sensor_write_reg(client, VSOC_SENSOR_WIDTH,
			      VSOC_SENSOR_DEF_WIDTH);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_HEIGHT,
			      VSOC_SENSOR_DEF_HEIGHT);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_FMT,
			      MEDIA_BUS_FMT_SRGGB10_1X10 & 0xFFFF);

	/* Step 10: Apply default control values to hardware (v4l2_ctrl_handler_setup) */
	ret = v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	if (ret) {
		dev_err(&client->dev, "failed to setup controls: %d\n", ret);
		goto err_subdev;
	}

	/* Step 11: Register subdev with v4l2_device */
	ret = v4l2_device_register_subdev(&sensor->v4l2_dev, &sensor->sd);
	if (ret)
		goto err_subdev;

	/* Step 12: Create /dev/v4l-subdevN */
	ret = v4l2_device_register_subdev_nodes(&sensor->v4l2_dev);
	if (ret)
		goto err_unreg_subdev;

	dev_info(&client->dev,
		 "v4l2_subdev '%s' registered (ctrl_handler + pad_ops + video_ops + core_ops)\n",
		 sensor->sd.name);
	return 0;

err_unreg_subdev:
	v4l2_device_unregister_subdev(&sensor->sd);
err_subdev:
	v4l2_subdev_cleanup(&sensor->sd);
err_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_v4l2_dev:
	v4l2_device_unregister(&sensor->v4l2_dev);
	return ret;
}

/*
 * vsoc_sensor_remove -- Part 5: Subdev Controls
 *
 * Step 1: Recover driver state
 * Step 2: Unregister subdev
 * Step 3: Clean up subdev state
 * Step 4: [NEW] Free control handler (v4l2_ctrl_handler_free)
 * Step 5: Clean up media entity
 * Step 6: Unregister v4l2_device
 *
 * Changes from Part 4:
 *   + Added v4l2_ctrl_handler_free (Step 4)
 */
static void vsoc_sensor_remove(struct i2c_client *client)
{
	/* Step 1: Recover driver state */
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	/* Step 2: Unregister subdev */
	v4l2_device_unregister_subdev(&sensor->sd);
	/* Step 3: Clean up subdev state */
	v4l2_subdev_cleanup(&sensor->sd);
	/* Step 4: Free control handler (v4l2_ctrl_handler_free) */
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	/* Step 5: Clean up media entity */
	media_entity_cleanup(&sensor->sd.entity);
	/* Step 6: Unregister v4l2_device */
	v4l2_device_unregister(&sensor->v4l2_dev);
	dev_info(&client->dev, "vsoc_sensor removed\n");
}

static const struct i2c_device_id vsoc_sensor_id[] = {
	{ "vsoc_sensor", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vsoc_sensor_id);

static struct i2c_driver vsoc_sensor_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe    = vsoc_sensor_probe,
	.remove   = vsoc_sensor_remove,
	.id_table = vsoc_sensor_id,
};

module_i2c_driver(vsoc_sensor_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor — Part 5: Subdev Controls");
MODULE_VERSION("1.0.0");
