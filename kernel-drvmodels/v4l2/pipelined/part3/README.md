# Part 3: v4l2_subdev_ops (Video + Core)

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces `v4l2_subdev_ops` -- the callback tables that allow a bridge driver to control sub-devices. Two ops categories are added: `v4l2_subdev_video_ops` with `s_stream` (start/stop sensor streaming) and `v4l2_subdev_core_ops` with `log_status` (dump sensor state). A test bridge module demonstrates calling these ops via `v4l2_subdev_call()`, which is the standard mechanism for bridge-to-subdev communication in the kernel.

## What You Build

Two kernel modules: (1) the sensor driver (`vsoc_sensor.ko`) now with `s_stream` and `log_status` callbacks that read/write sensor I2C registers, and (2) a test bridge module (`vsoc_test_bridge.ko`) that discovers the sensor subdev and exercises `v4l2_subdev_call()` to start streaming, log status, and stop streaming.

## Key APIs

- `v4l2_subdev_call()` -- invoke a subdev op from the bridge driver
- `.s_stream()` -- video op to start/stop sensor streaming
- `.log_status()` -- core op to dump device state to kernel log
- `i2c_smbus_write_word_data()` -- write a 16-bit value to a sensor register

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | Sensor driver with video_ops (.s_stream) and core_ops (.log_status) |
| vsoc_test_bridge.c | Test bridge: discovers sensor subdev, calls s_stream and log_status |
| test_stream.c | Userspace helper: prints expected dmesg output for verification |
| Makefile | Builds vsoc_sensor.ko, vsoc_test_bridge.ko, and test_stream binary |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko
sudo insmod vsoc_test_bridge.ko

# Test
sudo ./test_stream
dmesg | tail -20 | grep -E 'vsoc|sensor'

# Cleanup
sudo rmmod vsoc_test_bridge
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
# dmesg output:
vsoc_sensor 0-0010: VSOC-3000 sensor detected (ID: 0x3000)
vsoc_sensor 0-0010: v4l2_subdev 'vsoc_sensor' registered (video_ops + core_ops)
vsoc_bridge vsoc_bridge: found sensor subdev: 'vsoc_sensor'
vsoc_bridge vsoc_bridge: calling v4l2_subdev_call(sensor, video, s_stream, 1)
vsoc_sensor 0-0010: sensor streaming ON
vsoc_bridge vsoc_bridge: calling v4l2_subdev_call(sensor, core, log_status)
vsoc_sensor 0-0010: === Sensor Status ===
  Chip ID:   0x3000
  Status:    0x0003 (ready=1, streaming=1)
  Streaming: yes
vsoc_bridge vsoc_bridge: calling v4l2_subdev_call(sensor, video, s_stream, 0)
vsoc_sensor 0-0010: sensor streaming OFF
vsoc_bridge vsoc_bridge: v4l2_subdev_call test complete — check dmesg
```

## What's New vs Previous Part

- Added `v4l2_subdev_video_ops` with `.s_stream` callback
- Added `v4l2_subdev_core_ops` with `.log_status` callback
- Added I2C register write support (`vsoc_sensor_write_reg`)
- Added `streaming` state tracking in the sensor struct
- New test bridge module demonstrates `v4l2_subdev_call()` pattern

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering `v4l2_subdev_ops` categories, `v4l2_subdev_call()` macro, `s_stream` callback, and bridge-to-subdev communication.
