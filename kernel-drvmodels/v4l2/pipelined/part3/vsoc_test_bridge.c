// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Test Bridge — Part 3
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * A minimal kernel module that demonstrates v4l2_subdev_call().
 * It discovers the sensor subdev, calls s_stream(1), waits briefly,
 * calls log_status, then calls s_stream(0).
 *
 * This is NOT a real bridge driver — just a test harness showing
 * how a bridge driver communicates with subdevices.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "../hw/soc_hw_interface.h"

#define DRV_NAME "vsoc_test_bridge"

struct vsoc_test_bridge {
	struct v4l2_device v4l2_dev;
	struct v4l2_subdev *sensor_sd;
};

static struct vsoc_test_bridge *bridge;

/*
 * find_sensor_subdev — Part 3
 *
 * Synchronous I2C sensor discovery (replaced by async notifier in Part 7).
 *
 * Step 1: Get I2C adapter by number (i2c_get_adapter)
 * Step 2: Construct device name from adapter ID and I2C address
 * Step 3: Look up device on I2C bus (bus_find_device_by_name)
 * Step 4: Verify the device is an I2C client
 * Step 5: Extract v4l2_subdev from client data (i2c_get_clientdata)
 * Step 6: Release device reference (put_device)
 */
static struct v4l2_subdev *find_sensor_subdev(void)
{
	struct i2c_adapter *adap;
	struct i2c_client *client;
	struct v4l2_subdev *sd;
	struct device *dev;
	char name[32];

	/* Step 1: Get I2C adapter by number */
	adap = vsoc_hw_get_i2c_adapter();
	if (!adap)
		return NULL;

	/* Step 2: Construct device name from adapter ID and I2C address */
	snprintf(name, sizeof(name), "%d-%04x", adap->nr,
		 VSOC_SENSOR_I2C_ADDR);

	/* Step 3: Look up device on I2C bus */
	dev = bus_find_device_by_name(&i2c_bus_type, NULL, name);
	if (!dev)
		return NULL;

	/* Step 4: Verify the device is an I2C client */
	client = i2c_verify_client(dev);
	/* Step 6: Release device reference */
	put_device(dev);
	if (!client)
		return NULL;

	/* Step 5: Extract v4l2_subdev from client data */
	sd = i2c_get_clientdata(client);
	return sd;
}

/*
 * vsoc_test_bridge_probe — Part 3: v4l2_subdev_call Demo
 *
 * Demonstrates how a bridge discovers and operates a sensor subdev.
 * This is a teaching tool — real bridges use async binding (Part 7+).
 *
 * Step 1: Allocate bridge state
 * Step 2: Find sensor subdev via I2C bus lookup (find_sensor_subdev)
 * Step 3: Call v4l2_subdev_call(sensor, video, s_stream, 1) — start
 * Step 4: Sleep 100ms (let sensor "stream")
 * Step 5: Call v4l2_subdev_call(sensor, core, log_status) — dump state
 * Step 6: Call v4l2_subdev_call(sensor, video, s_stream, 0) — stop
 *
 * First appearance in Part 3.
 */
static int vsoc_test_bridge_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *sd;
	int ret;

	/* Step 1: Allocate bridge state */
	bridge = devm_kzalloc(&pdev->dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	/* Step 2: Find sensor subdev via I2C bus lookup */
	sd = find_sensor_subdev();
	if (!sd) {
		dev_err(&pdev->dev, "sensor subdev not found — load vsoc_sensor.ko first\n");
		return -ENODEV;
	}

	bridge->sensor_sd = sd;
	dev_info(&pdev->dev, "found sensor subdev: '%s'\n", sd->name);

	/* Step 3: Call s_stream(1) — start sensor */
	dev_info(&pdev->dev, "calling v4l2_subdev_call(sensor, video, s_stream, 1)\n");
	ret = v4l2_subdev_call(sd, video, s_stream, 1);
	if (ret)
		dev_warn(&pdev->dev, "s_stream(1) returned %d\n", ret);

	/* Step 4: Sleep 100ms — let sensor "stream" */
	msleep(100);

	/* Step 5: Call log_status — dump sensor state */
	dev_info(&pdev->dev, "calling v4l2_subdev_call(sensor, core, log_status)\n");
	v4l2_subdev_call(sd, core, log_status);

	/* Step 6: Call s_stream(0) — stop sensor */
	dev_info(&pdev->dev, "calling v4l2_subdev_call(sensor, video, s_stream, 0)\n");
	ret = v4l2_subdev_call(sd, video, s_stream, 0);
	if (ret)
		dev_warn(&pdev->dev, "s_stream(0) returned %d\n", ret);

	dev_info(&pdev->dev, "v4l2_subdev_call test complete — check dmesg\n");
	return 0;
}

static void vsoc_test_bridge_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "test bridge removed\n");
}

static struct platform_driver vsoc_test_bridge_driver = {
	.probe  = vsoc_test_bridge_probe,
	.remove = vsoc_test_bridge_remove,
	.driver = {
		.name = VSOC_BRIDGE_DEV_NAME,
	},
};

module_platform_driver(vsoc_test_bridge_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 Test Bridge — Part 3: v4l2_subdev_call demo");
MODULE_VERSION("1.0.0");
