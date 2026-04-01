// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual Camera Sensor — Part 2: v4l2_subdev Basics
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * NEW CONCEPT: v4l2_subdev initialization and registration
 *
 * This is a minimal I2C sensor driver that demonstrates:
 *   - I2C driver probe/remove lifecycle
 *   - Reading hardware chip ID via I2C (i2c_smbus_read_word_data)
 *   - Initializing a v4l2_subdev with v4l2_i2c_subdev_init()
 *   - Registering the subdev with a temporary v4l2_device
 *
 * The driver structure matches real kernel sensor drivers (imx219,
 * ov5640) — module_i2c_driver(), i2c_smbus_* for register access.
 *
 * Load order:
 *   1. insmod soc_hw_platform.ko   (creates virtual I2C bus + sensor)
 *   2. insmod vsoc_sensor.ko       (I2C driver probes the sensor)
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_sensor"

/* ====================================================================
 * Driver State
 * ==================================================================== */

struct vsoc_sensor {
	struct i2c_client *client;
	struct v4l2_subdev sd;

	/*
	 * Temporary v4l2_device for standalone operation.
	 * In Part 6, the bridge driver will own the v4l2_device and
	 * this field will be removed.
	 */
	struct v4l2_device v4l2_dev;
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
	int ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		dev_err(&client->dev, "read reg 0x%02x failed: %d\n",
			reg, ret);
	return ret;
}

/* ====================================================================
 * v4l2_subdev_ops — empty for Part 2 (added in Part 3)
 * ==================================================================== */

static const struct v4l2_subdev_ops vsoc_sensor_subdev_ops = {
};

/* ====================================================================
 * I2C Driver Probe / Remove
 * ==================================================================== */

/*
 * vsoc_sensor_probe -- Part 2: v4l2_subdev Basics
 *
 * Step 1: Allocate and initialize driver state (devm_kzalloc)
 * Step 2: Verify hardware -- read chip ID via I2C (expect 0x3000)
 * Step 3: Register temporary v4l2_device for standalone operation
 * Step 4: Initialize v4l2_subdev from I2C client (empty ops)
 * Step 5: Register subdev with v4l2_device
 *
 * First appearance -- no prior part to compare.
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
		dev_err(&client->dev,
			"unexpected chip ID: 0x%04x (expected 0x%04x)\n",
			chip_id, VSOC_SENSOR_CHIP_ID_VAL);
		return -ENODEV;
	}

	dev_info(&client->dev, "VSOC-3000 sensor detected (ID: 0x%04x)\n",
		 chip_id);

	/* Step 3: Register temporary v4l2_device for standalone operation */
	ret = v4l2_device_register(&client->dev, &sensor->v4l2_dev);
	if (ret) {
		dev_err(&client->dev, "v4l2_device_register failed: %d\n",
			ret);
		return ret;
	}

	/* Step 4: Initialize v4l2_subdev from I2C client */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vsoc_sensor_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sensor->sd.name, DRV_NAME, sizeof(sensor->sd.name));

	/* Step 5: Register subdev with v4l2_device */
	ret = v4l2_device_register_subdev(&sensor->v4l2_dev, &sensor->sd);
	if (ret) {
		dev_err(&client->dev, "register_subdev failed: %d\n", ret);
		goto err_v4l2;
	}

	dev_info(&client->dev, "v4l2_subdev '%s' registered\n",
		 sensor->sd.name);
	return 0;

err_v4l2:
	v4l2_device_unregister(&sensor->v4l2_dev);
	return ret;
}

/*
 * vsoc_sensor_remove -- Part 2: v4l2_subdev Basics
 *
 * Step 1: Recover driver state via i2c_get_clientdata + container_of
 * Step 2: Unregister subdev from v4l2_device
 * Step 3: Unregister v4l2_device
 *
 * First appearance.
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
MODULE_DESCRIPTION("VSOC-3000 Virtual Sensor — Part 2: v4l2_subdev Basics");
MODULE_VERSION("1.0.0");
