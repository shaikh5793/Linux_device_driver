// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor — Part 4: Pad Format Negotiation
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: v4l2_subdev_pad_ops (.get_fmt, .set_fmt, .enum_mbus_code)
 *
 * Builds on Part 3 by adding:
 *   - 1 source pad with media bus format negotiation
 *   - Two supported formats: SRGGB10_1X10 (Bayer RAW) and RGB888_1X24
 *   - Format clamping with width/height constraints
 *   - v4l2_subdev_state for modern pad format management
 *   - V4L2_SUBDEV_FL_HAS_DEVNODE for /dev/v4l-subdevN
 *   - v4l2_subdev_init_finalize() / v4l2_subdev_cleanup()
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
	struct v4l2_device v4l2_dev;
	struct media_pad pad;
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
	/* Clamp width: 160-3840, step 16 */
	fmt->width = clamp_t(u32, fmt->width,
			     VSOC_SENSOR_MIN_WIDTH, VSOC_SENSOR_MAX_WIDTH);
	fmt->width = rounddown(fmt->width, VSOC_SENSOR_STEP);

	/* Clamp height: 120-2160, step 16 */
	fmt->height = clamp_t(u32, fmt->height,
			      VSOC_SENSOR_MIN_HEIGHT, VSOC_SENSOR_MAX_HEIGHT);
	fmt->height = rounddown(fmt->height, VSOC_SENSOR_STEP);

	/* Validate mbus code */
	if (!vsoc_sensor_is_valid_mbus_code(fmt->code))
		fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;

	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

/* ====================================================================
 * v4l2_subdev_pad_ops — NEW in Part 4
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

/*
 * vsoc_sensor_set_fmt -- Part 4: Format Negotiation
 *
 * Step 1: Validate mbus code -- fall back to SRGGB10 if unsupported
 * Step 2: Clamp width to [160, 3840] aligned to 16
 * Step 3: Clamp height to [120, 2160] aligned to 16
 * Step 4: Set field to NONE, colorspace to RAW
 * Step 5: Get format slot from v4l2_subdev_state for this pad
 * Step 6: If ACTIVE format -- write width/height/code to sensor I2C registers
 * Step 7: Return clamped format to caller (never reject, always negotiate)
 *
 * First appearance in Part 4. V4L2 cardinal rule: clamp, never reject.
 */
static int vsoc_sensor_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	struct v4l2_mbus_framefmt *fmt;

	/* Step 1-4: Validate mbus code, clamp width/height, set field/colorspace */
	vsoc_sensor_clamp_format(&format->format);

	/* Step 5: Get format slot from v4l2_subdev_state for this pad */
	fmt = v4l2_subdev_state_get_format(state, format->pad);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	/* Step 6: If ACTIVE format -- write width/height/code to sensor I2C registers */
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

	/* Step 7: Return clamped format to caller (never reject, always negotiate) */
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
	.enum_mbus_code = vsoc_sensor_enum_mbus_code,
	.get_fmt        = vsoc_sensor_get_fmt,
	.set_fmt        = vsoc_sensor_set_fmt,
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
 * vsoc_sensor_probe -- Part 4: Pad Format Negotiation
 *
 * Step 1:  Allocate and initialize driver state
 * Step 2:  Verify hardware -- read chip ID via I2C
 * Step 3:  Register temporary v4l2_device
 * Step 4:  Initialize v4l2_subdev with video_ops + core_ops + pad_ops
 * Step 5:  [NEW] Set internal_ops (.init_state) for default format
 * Step 6:  [NEW] Initialize media entity with 1 source pad (MEDIA_PAD_FL_SOURCE)
 * Step 7:  [NEW] Allocate subdev state via v4l2_subdev_init_finalize
 * Step 8:  [NEW] Write default format (1920x1080 SRGGB10) to sensor registers
 * Step 9:  Register subdev with v4l2_device
 * Step 10: [NEW] Create /dev/v4l-subdevN via v4l2_device_register_subdev_nodes
 *
 * Changes from Part 3:
 *   + Added pad_ops to subdev_ops (enum_mbus_code, get_fmt, set_fmt)
 *   + Added internal_ops with .init_state callback (Step 5)
 *   + Added media entity pad initialization (Step 6)
 *   + Added v4l2_subdev_init_finalize for state management (Step 7)
 *   + Added hardware default format writes (Step 8)
 *   + Added subdev device node creation (Step 10)
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

	/* Step 4: Initialize v4l2_subdev with video_ops + core_ops + pad_ops */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vsoc_sensor_subdev_ops);
	/* Step 5: Set internal_ops (.init_state) for default format */
	sensor->sd.internal_ops = &vsoc_sensor_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sensor->sd.name, DRV_NAME, sizeof(sensor->sd.name));

	/* Step 6: Initialize media entity with 1 source pad (MEDIA_PAD_FL_SOURCE) */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity,
				     VSOC_SENSOR_NUM_PADS, &sensor->pad);
	if (ret) {
		v4l2_device_unregister(&sensor->v4l2_dev);
		return ret;
	}

	/* Step 7: Allocate subdev state via v4l2_subdev_init_finalize */
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		media_entity_cleanup(&sensor->sd.entity);
		v4l2_device_unregister(&sensor->v4l2_dev);
		return ret;
	}

	/* Step 8: Write default format (1920x1080 SRGGB10) to sensor registers */
	vsoc_sensor_write_reg(client, VSOC_SENSOR_WIDTH,
			      VSOC_SENSOR_DEF_WIDTH);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_HEIGHT,
			      VSOC_SENSOR_DEF_HEIGHT);
	vsoc_sensor_write_reg(client, VSOC_SENSOR_FMT,
			      MEDIA_BUS_FMT_SRGGB10_1X10 & 0xFFFF);

	/* Step 9: Register subdev with v4l2_device */
	ret = v4l2_device_register_subdev(&sensor->v4l2_dev, &sensor->sd);
	if (ret) {
		v4l2_subdev_cleanup(&sensor->sd);
		media_entity_cleanup(&sensor->sd.entity);
		v4l2_device_unregister(&sensor->v4l2_dev);
		return ret;
	}

	/* Step 10: Create /dev/v4l-subdevN via v4l2_device_register_subdev_nodes */
	ret = v4l2_device_register_subdev_nodes(&sensor->v4l2_dev);
	if (ret) {
		v4l2_device_unregister_subdev(&sensor->sd);
		v4l2_subdev_cleanup(&sensor->sd);
		media_entity_cleanup(&sensor->sd.entity);
		v4l2_device_unregister(&sensor->v4l2_dev);
		return ret;
	}

	dev_info(&client->dev,
		 "v4l2_subdev '%s' registered (pad_ops + video_ops + core_ops)\n",
		 sensor->sd.name);
	return 0;
}

/*
 * vsoc_sensor_remove -- Part 4: Pad Format Negotiation
 *
 * Step 1: Recover driver state
 * Step 2: Unregister subdev from v4l2_device
 * Step 3: [NEW] Clean up subdev state (v4l2_subdev_cleanup)
 * Step 4: [NEW] Clean up media entity
 * Step 5: Unregister v4l2_device
 *
 * Changes from Part 3:
 *   + Added v4l2_subdev_cleanup (Step 3)
 *   + Added media_entity_cleanup (Step 4)
 */
static void vsoc_sensor_remove(struct i2c_client *client)
{
	/* Step 1: Recover driver state */
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	/* Step 2: Unregister subdev from v4l2_device */
	v4l2_device_unregister_subdev(&sensor->sd);
	/* Step 3: Clean up subdev state (v4l2_subdev_cleanup) */
	v4l2_subdev_cleanup(&sensor->sd);
	/* Step 4: Clean up media entity */
	media_entity_cleanup(&sensor->sd.entity);
	/* Step 5: Unregister v4l2_device */
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
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor — Part 4: Pad Format Negotiation");
MODULE_VERSION("1.0.0");
