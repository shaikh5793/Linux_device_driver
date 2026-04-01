// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor — Part 3: v4l2_subdev_ops
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: v4l2_subdev_video_ops (.s_stream), v4l2_subdev_core_ops (.log_status)
 *
 * Builds on Part 2 by adding:
 *   - v4l2_subdev_video_ops with s_stream callback
 *   - v4l2_subdev_core_ops with log_status callback
 *   - Writing to sensor hardware registers via I2C on stream start/stop
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko
 *   2. insmod vsoc_sensor.ko
 *   3. insmod vsoc_test_bridge.ko  (calls v4l2_subdev_call)
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_sensor"

struct vsoc_sensor {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct v4l2_device v4l2_dev;
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
 * v4l2_subdev_video_ops — NEW in Part 3
 *
 * s_stream: Called by the bridge driver to start/stop sensor streaming.
 * This is the primary control point for the sensor hardware.
 * ==================================================================== */

/*
 * vsoc_sensor_s_stream -- Part 3: Stream Control
 *
 * Step 1: Recover driver state from subdev via container_of
 * Step 2: If enabling -- write ENABLE|STREAM bits to sensor CTRL register via I2C
 * Step 3: If disabling -- clear CTRL register (write 0x0000)
 * Step 4: Update software streaming state shadow
 *
 * First appearance in Part 3.
 */
static int vsoc_sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	/* Step 1: Recover driver state from subdev via container_of */
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);
	u16 ctrl;
	int ret;

	/* Step 2: If enabling -- write ENABLE|STREAM bits to CTRL register */
	if (enable) {
		ctrl = VSOC_SENSOR_CTRL_ENABLE | VSOC_SENSOR_CTRL_STREAM;
		ret = vsoc_sensor_write_reg(sensor->client,
					    VSOC_SENSOR_CTRL, ctrl);
		if (ret)
			return ret;

		/* Step 4: Update software streaming state shadow */
		sensor->streaming = true;
		dev_info(&sensor->client->dev, "sensor streaming ON\n");
	} else {
		/* Step 3: If disabling -- clear CTRL register (write 0x0000) */
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
 * v4l2_subdev_core_ops — NEW in Part 3
 *
 * log_status: Dumps sensor state to the kernel log for debugging.
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
};

/* ====================================================================
 * I2C Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_sensor_probe -- Part 3: v4l2_subdev_ops
 *
 * Step 1: Allocate and initialize driver state
 * Step 2: Verify hardware -- read chip ID via I2C
 * Step 3: Register temporary v4l2_device
 * Step 4: Initialize v4l2_subdev with video_ops + core_ops
 * Step 5: Register subdev with v4l2_device
 *
 * Changes from Part 2:
 *   ~ Step 4: subdev_ops now includes video_ops (.s_stream) and core_ops (.log_status)
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

	/* Step 3: Register temporary v4l2_device */
	ret = v4l2_device_register(&client->dev, &sensor->v4l2_dev);
	if (ret)
		return ret;

	/* Step 4: Initialize v4l2_subdev with video_ops + core_ops */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vsoc_sensor_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sensor->sd.name, DRV_NAME, sizeof(sensor->sd.name));

	/* Step 5: Register subdev with v4l2_device */
	ret = v4l2_device_register_subdev(&sensor->v4l2_dev, &sensor->sd);
	if (ret) {
		v4l2_device_unregister(&sensor->v4l2_dev);
		return ret;
	}

	dev_info(&client->dev,
		 "v4l2_subdev '%s' registered (video_ops + core_ops)\n",
		 sensor->sd.name);
	return 0;
}

/*
 * vsoc_sensor_remove -- Part 3: v4l2_subdev_ops
 *
 * Step 1: Recover driver state via i2c_get_clientdata + container_of
 * Step 2: Unregister subdev from v4l2_device
 * Step 3: Unregister v4l2_device
 *
 * Same as Part 2.
 */
static void vsoc_sensor_remove(struct i2c_client *client)
{
	/* Step 1: Recover driver state via i2c_get_clientdata + container_of */
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vsoc_sensor *sensor = to_vsoc_sensor(sd);

	/* Step 2: Unregister subdev from v4l2_device */
	v4l2_device_unregister_subdev(&sensor->sd);
	/* Step 3: Unregister v4l2_device */
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
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor — Part 3: v4l2_subdev_ops");
MODULE_VERSION("1.0.0");
