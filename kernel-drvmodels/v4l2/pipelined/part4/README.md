# Part 4: Pad Format Negotiation

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces `v4l2_subdev_pad_ops` -- the mechanism for negotiating media bus formats between pipeline elements. Each sub-device exposes pads (source/sink endpoints) and the pad ops allow userspace and other drivers to enumerate supported formats, get the current format, and set a new format. This part also introduces `v4l2_subdev_state` for modern pad format management, `media_entity_pads_init()` for media entity setup, and `v4l2_subdev_init_finalize()` for state initialization. The sensor creates a `/dev/v4l-subdevN` device node for direct userspace access.

## What You Build

An enhanced sensor driver (`vsoc_sensor.ko`) with one source pad supporting two media bus formats: SRGGB10_1X10 (Bayer RAW 10-bit) and RGB888_1X24 (24-bit RGB). Format requests are clamped to hardware constraints (160-3840 width, 120-2160 height, step 16). Active format changes are written to sensor I2C registers. The test program exercises the subdev ioctls through `/dev/v4l-subdevN`.

## Key APIs

- `.enum_mbus_code()` -- enumerate supported media bus formats
- `.get_fmt()` -- get current pad format
- `.set_fmt()` -- set pad format (with clamping)
- `v4l2_subdev_state_get_format()` -- access format from subdev state
- `v4l2_subdev_init_finalize()` -- finalize subdev state initialization
- `media_entity_pads_init()` -- initialize media entity pads
- `v4l2_device_register_subdev_nodes()` -- create /dev/v4l-subdevN nodes

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | Sensor driver with pad_ops: enum_mbus_code, get_fmt, set_fmt |
| test_subdev.c | Userspace test: exercises VIDIOC_SUBDEV_ENUM_MBUS_CODE, G_FMT, S_FMT |
| Makefile | Builds vsoc_sensor.ko and test_subdev binary |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko

# Test
sudo ./test_subdev

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Part 4: Pad Format Negotiation Test ===
Found vsoc_sensor at /dev/v4l-subdev0

=== Enumerating Media Bus Codes ===
  [0] code=0x300f  SRGGB10_1X10 (Bayer RAW 10-bit)
  [1] code=0x1022  RGB888_1X24 (24-bit RGB)
  Total: 2 formats

=== Get Current Format (pad 0) ===
  Width:      1920
  Height:     1080
  Code:       0x300f  SRGGB10_1X10 (Bayer RAW 10-bit)
  Field:      0
  Colorspace: 9

=== Set Format: 1280x720 SRGGB10 ===
  Result: 1280x720 code=0x300f  SRGGB10_1X10 (Bayer RAW 10-bit)

=== Verify: Get Format After Set ===
  Width:  1280 (expected 1280)
  Height: 720 (expected 720)
  Code:   0x300f  SRGGB10_1X10 (Bayer RAW 10-bit)
  PASS: format matches

=== Test PASSED ===
```

## What's New vs Previous Part

- Added `v4l2_subdev_pad_ops` (`.enum_mbus_code`, `.get_fmt`, `.set_fmt`)
- Added `media_pad` and `media_entity_pads_init()` for 1 source pad
- Added `v4l2_subdev_internal_ops` with `.init_state` for default format
- Added `v4l2_subdev_init_finalize()` / `v4l2_subdev_cleanup()` lifecycle
- Added `/dev/v4l-subdevN` device node via `v4l2_device_register_subdev_nodes()`
- Format clamping with width/height constraints and step alignment
- Two supported media bus formats: SRGGB10_1X10 and RGB888_1X24
- Test bridge module removed (pad ops tested directly from userspace)

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering pad-level format negotiation, `v4l2_subdev_pad_ops`, media bus format codes, `v4l2_subdev_state`, and media entity initialization.
