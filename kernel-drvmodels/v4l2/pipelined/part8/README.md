# Part 8: Media Controller Basics

Copyright (c) 2024 TECH VEDA. Author: Raghu Bharadwaj.

## Concept

This part introduces the **Media Controller** framework. A `media_device` is embedded in the bridge and initialized with `media_device_init()`. The sensor subdev gets a `media_pad` (source) and its entity function is set to `MEDIA_ENT_F_CAM_SENSOR`. The bridge links `v4l2_dev.mdev` to the media device so that all subdevs and video nodes automatically become media entities. After async binding completes, `media_device_register()` exposes `/dev/mediaN` to userspace.

## What You Build

- **vsoc_bridge.ko** -- Platform bridge driver that owns `v4l2_device`, `media_device`, VB2 queue, and video node. Waits for the sensor via async notifier.
- **vsoc_sensor.ko** -- I2C sensor subdev with 1 source pad, V4L2 controls, pad format ops, and `MEDIA_ENT_F_CAM_SENSOR` entity function.
- **test_mc** -- Userspace test that opens `/dev/media0`, calls `MEDIA_IOC_DEVICE_INFO` and `MEDIA_IOC_ENUM_ENTITIES`.

## Key APIs

- `media_device_init()` -- initialize media device before use
- `media_device_register()` -- create /dev/mediaN device node
- `media_entity_pads_init()` -- attach pads to a media entity
- `MEDIA_IOC_DEVICE_INFO` -- query media device info from userspace
- `MEDIA_IOC_ENUM_ENTITIES` -- enumerate entities from userspace

## Files

| File | Description |
|------|-------------|
| vsoc_bridge.c | Bridge with embedded media_device; registers media device in .complete |
| vsoc_sensor.c | Sensor subdev with 1 source media_pad and CAM_SENSOR entity function |
| test_mc.c | Userspace test: MEDIA_IOC_DEVICE_INFO + MEDIA_IOC_ENUM_ENTITIES |
| Makefile | Builds vsoc_sensor.ko, vsoc_bridge.ko, test_mc |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_bridge.ko
sudo insmod vsoc_sensor.ko    # loaded last — triggers async binding

# Test
sudo ./test_mc

# v4l2-ctl / media-ctl commands
media-ctl -d /dev/media0 -p                                            # media topology
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes                 # sensor formats
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f  # frame size range
v4l2-ctl -d /dev/v4l-subdev0 -L                                       # sensor controls

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod vsoc_bridge
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Media Controller Test (Part 8) ===

Media Device Info:
  Driver:   vsoc_bridge
  Model:    VSOC-3000 Camera
  Bus Info: platform:vsoc_bridge
  HW Rev:   0x00000100

Entities:
  ID     Name                           Type                 Pads
  1      vsoc_sensor                    CAM_SENSOR           1
  2      VSOC-3000 Capture              IO_V4L               0

Done. Found entities above.
```

## What's New vs Previous Part

- Bridge embeds `struct media_device` and calls `media_device_init()` / `media_device_register()`
- `v4l2_dev.mdev` linked to the media device
- Sensor has `struct media_pad` with `MEDIA_PAD_FL_SOURCE`
- Sensor entity function set to `MEDIA_ENT_F_CAM_SENSOR`
- `media_entity_pads_init()` called in sensor probe
- `media_device_unregister()` / `media_device_cleanup()` in bridge remove
- New userspace test exercises media controller ioctls
