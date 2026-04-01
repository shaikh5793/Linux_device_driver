# Part 2: v4l2_subdev Basics

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces the `v4l2_subdev` abstraction, the foundation of the V4L2 sub-device framework. A sub-device represents a single hardware block (sensor, ISP, CSI receiver) inside a camera pipeline. Here you learn how an I2C sensor driver probes hardware, verifies the chip ID over the I2C bus, and registers a `v4l2_subdev` with a `v4l2_device`. The subdev ops struct is empty in this part -- ops are added incrementally in Parts 3-5.

## What You Build

A minimal I2C sensor driver (`vsoc_sensor.ko`) that probes the VSOC-3000 virtual sensor at I2C address 0x10, reads the chip ID register (expects 0x3000), and registers an empty `v4l2_subdev`. The sensor owns a temporary `v4l2_device` for standalone operation.

## Key APIs

- `i2c_smbus_read_word_data()` -- read a 16-bit sensor register over I2C
- `v4l2_device_register()` -- register a V4L2 device (top-level container)
- `v4l2_i2c_subdev_init()` -- initialize a subdev from an I2C client
- `v4l2_device_register_subdev()` -- attach a subdev to a v4l2_device
- `module_i2c_driver()` -- macro to register an I2C driver

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | I2C sensor driver: probe, chip ID check, v4l2_subdev init and registration |
| test_sensor.c | Userspace test: verifies sensor I2C device exists in sysfs |
| Makefile | Builds vsoc_sensor.ko and test_sensor binary |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko

# Test
sudo ./test_sensor
dmesg | grep vsoc_sensor

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
# dmesg output:
vsoc_sensor 0-0010: VSOC-3000 sensor detected (ID: 0x3000)
vsoc_sensor 0-0010: v4l2_subdev 'vsoc_sensor' registered

# test_sensor output:
=== Part 2: v4l2_subdev Basics Test ===
PASS: Found sensor I2C device: 0-0010
=== Results: 1 passed, 0 failed ===
```

## What's New vs Previous Part

This is the first part in the series. It establishes the baseline:
- I2C driver lifecycle (`probe`/`remove`)
- Chip ID verification via `i2c_smbus_read_word_data()`
- `v4l2_subdev` initialization and registration
- Temporary `v4l2_device` owned by the sensor (removed in Part 6)

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering V4L2 sub-device model, `v4l2_subdev` structure, I2C sensor driver skeleton, and `v4l2_device` registration.
