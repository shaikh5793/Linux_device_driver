// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor -- Part 10: Pipeline Validation
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * Same as Part 9 sensor (no changes needed for Part 10).
 * The sensor has 1 source pad and registers as MEDIA_ENT_F_CAM_SENSOR.
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_csi2.ko
 *   3. insmod vsoc_bridge.ko
 *   4. insmod vsoc_sensor.ko
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-async.h>
#include <media/media-entity.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_sensor"

/* Supported media bus formats */
static const u32 vsoc_sensor_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SRGGB8_1X8,
};

/* ====================================================================
 * Driver State
 * ==================================================================== */

struct vsoc_sensor {
	struct i2c_client	*client;
	struct v4l2_subdev	sd;
	struct media_pad	pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_mbus_framefmt fmt;
	bool			streaming;
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
 * v4l2_subdev_video_ops
 * ==================================================================== */

static int vsoc_sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	u16 ctrl;
	int ret;

	if (enable) {
		vsoc_sensor_write_reg(sensor->client, VSOC_SENSOR_WIDTH,
				      sensor->fmt.width);
		vsoc_sensor_write_reg(sensor->client, VSOC_SENSOR_HEIGHT,
				      sensor->fmt.height);

		ctrl = VSOC_SENSOR_CTRL_ENABLE | VSOC_SENSOR_CTRL_STREAM;
		ret = vsoc_sensor_write_reg(sensor->client,
					    VSOC_SENSOR_CTRL, ctrl);
		if (ret)
			return ret;

		sensor->streaming = true;
		dev_info(&sensor->client->dev, "sensor streaming ON (%ux%u)\n",
			 sensor->fmt.width, sensor->fmt.height);
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
	unsigned int i;
	bool valid = false;

	if (fse->index > 0)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(vsoc_sensor_mbus_codes); i++) {
		if (fse->code == vsoc_sensor_mbus_codes[i]) {
			valid = true;
			break;
		}
	}
	if (!valid)
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
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	format->format = sensor->fmt;
	return 0;
}

static int vsoc_sensor_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	bool code_valid = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vsoc_sensor_mbus_codes); i++) {
		if (fmt->code == vsoc_sensor_mbus_codes[i]) {
			code_valid = true;
			break;
		}
	}
	if (!code_valid)
		fmt->code = vsoc_sensor_mbus_codes[0];

	fmt->width  = clamp_t(u32, fmt->width,
			      VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	fmt->width  = rounddown(fmt->width, VSOC_SENSOR_STEP);
	fmt->height = rounddown(fmt->height, VSOC_SENSOR_STEP);

	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->fmt = *fmt;

	return 0;
}

static const struct v4l2_subdev_pad_ops vsoc_sensor_pad_ops = {
	.enum_mbus_code  = vsoc_sensor_enum_mbus_code,
	.enum_frame_size = vsoc_sensor_enum_frame_size,
	.get_fmt         = vsoc_sensor_get_fmt,
	.set_fmt         = vsoc_sensor_set_fmt,
};

/* ====================================================================
 * v4l2_subdev_core_ops
 * ==================================================================== */

static int vsoc_sensor_log_status(struct v4l2_subdev *sd)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	int status;

	status = vsoc_sensor_read_reg(sensor->client, VSOC_SENSOR_STATUS);

	dev_info(&sensor->client->dev,
		 "=== Sensor Status ===\n"
		 "  Format:    %ux%u code=0x%04x\n"
		 "  HW Status: 0x%04x (ready=%d, streaming=%d)\n"
		 "  Streaming: %s\n",
		 sensor->fmt.width, sensor->fmt.height, sensor->fmt.code,
		 status,
		 !!(status & VSOC_SENSOR_STATUS_READY),
		 !!(status & VSOC_SENSOR_STATUS_STREAMING),
		 sensor->streaming ? "yes" : "no");

	return 0;
}

static const struct v4l2_subdev_core_ops vsoc_sensor_core_ops = {
	.log_status = vsoc_sensor_log_status,
};

static const struct v4l2_subdev_ops vsoc_sensor_subdev_ops = {
	.core  = &vsoc_sensor_core_ops,
	.video = &vsoc_sensor_video_ops,
	.pad   = &vsoc_sensor_pad_ops,
};

/* ====================================================================
 * V4L2 Controls
 * ==================================================================== */

static int vsoc_sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vsoc_sensor *sensor =
		container_of(ctrl->handler, struct vsoc_sensor, ctrl_handler);
	u16 reg;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		reg = VSOC_SENSOR_EXPOSURE;
		break;
	case V4L2_CID_GAIN:
		reg = VSOC_SENSOR_GAIN;
		break;
	case V4L2_CID_DIGITAL_GAIN:
		reg = VSOC_SENSOR_DGAIN;
		break;
	case V4L2_CID_HFLIP:
		reg = VSOC_SENSOR_HFLIP;
		break;
	case V4L2_CID_VFLIP:
		reg = VSOC_SENSOR_VFLIP;
		break;
	case V4L2_CID_TEST_PATTERN:
		reg = VSOC_SENSOR_TESTPAT;
		break;
	default:
		return -EINVAL;
	}

	return vsoc_sensor_write_reg(sensor->client, reg, ctrl->val);
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

	v4l2_ctrl_handler_init(hdl, 6);

	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_EXPOSURE, 0,
			  VSOC_SENSOR_MAX_EXPOSURE, 1,
			  VSOC_SENSOR_DEF_EXPOSURE);

	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_GAIN, 0,
			  VSOC_SENSOR_MAX_GAIN, 1,
			  VSOC_SENSOR_DEF_GAIN);

	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN, 0, 255, 1, 64);

	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(hdl, &vsoc_sensor_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(hdl, &vsoc_sensor_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(vsoc_test_pattern_menu) - 1,
				     0, 0, vsoc_test_pattern_menu);

	if (hdl->error)
		return hdl->error;

	sensor->sd.ctrl_handler = hdl;
	return 0;
}

/* ====================================================================
 * I2C Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_sensor_probe -- Part 10: Pipeline Validation
 *
 * Identical to Part 8 -- sensor is stable from Part 8 onward.
 * Pipeline growth happens in bridge, CSI-2, and ISP modules.
 *
 * Step 1: Allocate and initialize driver state
 * Step 2: Verify hardware -- read chip ID via I2C
 * Step 3: Set default format in driver struct (sensor->fmt)
 * Step 4: Initialize v4l2_subdev with all ops
 * Step 5: Initialize media entity with 1 source pad
 * Step 6: Set entity function to MEDIA_ENT_F_CAM_SENSOR
 * Step 7: Initialize V4L2 controls
 * Step 8: Register with async framework
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

	/* Step 3: Set default format in driver struct (sensor->fmt) */
	sensor->fmt.width  = VSOC_SENSOR_DEF_WIDTH;
	sensor->fmt.height = VSOC_SENSOR_DEF_HEIGHT;
	sensor->fmt.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
	sensor->fmt.field  = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_RAW;

	/* Step 4: Initialize v4l2_subdev with all ops */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vsoc_sensor_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sensor->sd.name, DRV_NAME, sizeof(sensor->sd.name));

	/* Step 5: Initialize media entity with 1 source pad */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret) {
		dev_err(&client->dev, "media_entity_pads_init failed: %d\n",
			ret);
		return ret;
	}
	/* Step 6: Set entity function to MEDIA_ENT_F_CAM_SENSOR */
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Step 7: Initialize V4L2 controls */
	ret = vsoc_sensor_init_controls(sensor);
	if (ret) {
		dev_err(&client->dev, "init controls failed: %d\n", ret);
		goto err_entity;
	}

	/* Step 8: Register with async framework */
	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(&client->dev, "async register failed: %d\n", ret);
		goto err_ctrl;
	}

	dev_info(&client->dev,
		 "v4l2_subdev '%s' registered (media entity: CAM_SENSOR)\n",
		 sensor->sd.name);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

/*
 * vsoc_sensor_remove -- Part 10: Pipeline Validation
 *
 * Identical to Part 8 -- sensor is stable from Part 8 onward.
 * Pipeline growth happens in bridge, CSI-2, and ISP modules.
 *
 * Step 1: Recover driver state
 * Step 2: Unregister from async framework
 * Step 3: Free control handler
 * Step 4: Clean up media entity
 */
static void vsoc_sensor_remove(struct i2c_client *client)
{
	/* Step 1: Recover driver state */
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	/* Step 2: Unregister from async framework */
	v4l2_async_unregister_subdev(&sensor->sd);
	/* Step 3: Free control handler */
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	/* Step 4: Clean up media entity */
	media_entity_cleanup(&sensor->sd.entity);

	dev_info(&client->dev, "vsoc_sensor removed\n");
}

/* ====================================================================
 * I2C Driver Registration
 * ==================================================================== */

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
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor -- Part 10: Pipeline Validation");
MODULE_VERSION("1.0.0");
