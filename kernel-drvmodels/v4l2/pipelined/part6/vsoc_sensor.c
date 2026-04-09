// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor — Part 6: Bridge-Owned Subdev
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: Sensor without its own v4l2_device — the bridge owns it.
 *
 * Builds on Part 5 (controls + pad ops) but removes the temporary
 * v4l2_device.  The bridge driver discovers the sensor by looking up
 * the I2C client and registers the subdev itself.
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_sensor.ko   (probe only inits subdev)
 *   3. insmod vsoc_bridge.ko   (finds sensor, registers subdev)
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

/* Supported media bus formats */
static const u32 vsoc_sensor_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_RGB888_1X24,
};

struct vsoc_sensor {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrl_handler;
	struct media_pad pad;
	bool streaming;

	/* Controls */
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *test_pattern;
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
 * V4L2 Control Operations
 * ==================================================================== */

static int vsoc_sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vsoc_sensor *sensor =
		container_of(ctrl->handler, struct vsoc_sensor, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		return vsoc_sensor_write_reg(sensor->client,
					     VSOC_SENSOR_EXPOSURE, ctrl->val);
	case V4L2_CID_GAIN:
		return vsoc_sensor_write_reg(sensor->client,
					     VSOC_SENSOR_GAIN, ctrl->val);
	case V4L2_CID_HFLIP:
		return vsoc_sensor_write_reg(sensor->client,
					     VSOC_SENSOR_HFLIP, ctrl->val);
	case V4L2_CID_VFLIP:
		return vsoc_sensor_write_reg(sensor->client,
					     VSOC_SENSOR_VFLIP, ctrl->val);
	case V4L2_CID_TEST_PATTERN:
		return vsoc_sensor_write_reg(sensor->client,
					     VSOC_SENSOR_TESTPAT, ctrl->val);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops vsoc_sensor_ctrl_ops = {
	.s_ctrl = vsoc_sensor_s_ctrl,
};

static const char * const vsoc_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Gradient",
	"Checkerboard",
	"White",
};

static int vsoc_sensor_init_controls(struct vsoc_sensor *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, 5);
	if (ret)
		return ret;

	sensor->exposure = v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
					     V4L2_CID_EXPOSURE, 0,
					     VSOC_SENSOR_MAX_EXPOSURE, 1,
					     VSOC_SENSOR_DEF_EXPOSURE);

	sensor->gain = v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
					 V4L2_CID_GAIN, 0,
					 VSOC_SENSOR_MAX_GAIN, 1,
					 VSOC_SENSOR_DEF_GAIN);

	sensor->hflip = v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);

	sensor->vflip = v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	sensor->test_pattern = v4l2_ctrl_new_std_menu_items(hdl,
					&vsoc_sensor_ctrl_ops,
					V4L2_CID_TEST_PATTERN,
					ARRAY_SIZE(vsoc_test_pattern_menu) - 1,
					0, 0, vsoc_test_pattern_menu);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;
}

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
 * v4l2_subdev_pad_ops
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
 * v4l2_subdev_video_ops
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
 * v4l2_subdev_core_ops
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

/* Combine all ops */
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
 *
 * Key difference from Part 5: NO v4l2_device registration here.
 * The bridge driver will own the v4l2_device and register this subdev.
 * ==================================================================== */

/*
 * vsoc_sensor_probe -- Part 6: Bridge-Owned Subdev
 *
 * Step 1: Allocate and initialize driver state
 * Step 2: Verify hardware -- read chip ID via I2C
 * Step 3: [REMOVED] No v4l2_device registration -- bridge owns it now
 * Step 4: Initialize v4l2_subdev with all ops (bridge will register it)
 * Step 5: Set internal_ops (.init_state)
 * Step 6: Initialize media entity with 1 source pad
 * Step 7: Allocate subdev state via v4l2_subdev_init_finalize
 * Step 8: Initialize V4L2 controls
 * Step 9: Write default format to sensor registers
 *
 * Changes from Part 5:
 *   - Removed v4l2_device_register (bridge owns v4l2_device)
 *   - Removed v4l2_device_register_subdev (bridge will do it)
 *   - Removed v4l2_device_register_subdev_nodes (bridge will do it)
 *   - Removed v4l2_ctrl_handler_setup (bridge applies after binding)
 *   Sensor is now a "leaf" -- it initializes itself and waits.
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

	/* Step 4: Initialize v4l2_subdev with all ops (bridge will register it) */
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
		return ret;

	/* Step 7: Allocate subdev state via v4l2_subdev_init_finalize */
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		media_entity_cleanup(&sensor->sd.entity);
		return ret;
	}

	/* Step 8: Initialize V4L2 controls */
	ret = vsoc_sensor_init_controls(sensor);
	if (ret) {
		v4l2_subdev_cleanup(&sensor->sd);
		media_entity_cleanup(&sensor->sd.entity);
		return ret;
	}

	/* Step 9: Write default format to sensor registers */
	vsoc_sensor_write_reg(client, VSOC_SENSOR_WIDTH,
			      VSOC_SENSOR_DEF_WIDTH);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_HEIGHT,
			      VSOC_SENSOR_DEF_HEIGHT);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_FMT,
			      MEDIA_BUS_FMT_SRGGB10_1X10 & 0xFFFF);

	dev_info(&client->dev,
		 "v4l2_subdev '%s' initialized (waiting for bridge)\n",
		 sensor->sd.name);
	return 0;
}

/*
 * vsoc_sensor_remove -- Part 6: Bridge-Owned Subdev
 *
 * Step 1: Recover driver state
 * Step 2: [REMOVED] No v4l2_device_unregister_subdev (not our registration)
 * Step 3: Free control handler
 * Step 4: Clean up subdev state
 * Step 5: Clean up media entity
 *
 * Changes from Part 5:
 *   - Removed v4l2_device_unregister_subdev
 *   - Removed v4l2_device_unregister (bridge owns it)
 */
static void vsoc_sensor_remove(struct i2c_client *client)
{
	/* Step 1: Recover driver state */
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	/* Step 3: Free control handler */
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	/* Step 4: Clean up subdev state */
	v4l2_subdev_cleanup(&sensor->sd);
	/* Step 5: Clean up media entity */
	media_entity_cleanup(&sensor->sd.entity);
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
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor — Part 6: Bridge-Owned Subdev");
MODULE_VERSION("1.0.0");
