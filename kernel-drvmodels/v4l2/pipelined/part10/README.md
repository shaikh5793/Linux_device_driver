# Part 10: Pipeline Validation

Copyright (c) 2024 TECH VEDA. Author: Raghu Bharadwaj.

## Concept

This part introduces **pipeline validation** via `media_pipeline_start()` and `media_pipeline_stop()`. When `VIDIOC_STREAMON` is called, the bridge calls `media_pipeline_start()` which walks all enabled links and invokes `.link_validate` on each sink pad. If any format mismatch is detected (e.g., sensor is 1280x720 but CSI-2 expects 1920x1080), the call returns `-EPIPE` and streaming is refused. This ensures end-to-end format consistency before any hardware is started.

## What You Build

- **vsoc_bridge.ko** -- Bridge now has `struct media_pipeline`, calls `media_pipeline_start/stop()` around streaming, and implements `link_validate` on the video node's sink pad via `media_entity_operations`.
- **vsoc_csi2.ko** -- CSI-2 adds `.link_validate = v4l2_subdev_link_validate_default` on its pad ops, checking that upstream source format matches its sink format.
- **vsoc_sensor.ko** -- Same as Part 9 (unchanged).
- **test_validate** -- Two-phase test: (1) matching formats should stream successfully, (2) mismatched formats should fail with `EPIPE`.

## Key APIs

- `media_pipeline_start()` -- validate and start the media pipeline
- `media_pipeline_stop()` -- stop the media pipeline
- `v4l2_subdev_link_validate_default()` -- default subdev link validation (width/height/code match)
- `.link_validate` in `media_entity_operations` -- custom validation on video node sink pad

## Files

| File | Description |
|------|-------------|
| vsoc_bridge.c | Bridge with media_pipeline_start/stop and video node link_validate |
| vsoc_sensor.c | Sensor subdev (same as Part 9) |
| vsoc_csi2.c | CSI-2 with v4l2_subdev_link_validate_default on sink pad |
| test_validate.c | Two-phase test: matching formats (PASS) and mismatched formats (EPIPE) |
| Makefile | Builds vsoc_sensor.ko, vsoc_csi2.ko, vsoc_bridge.ko, test_validate |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_csi2.ko
sudo insmod vsoc_bridge.ko
sudo insmod vsoc_sensor.ko    # loaded last — triggers async binding

# Test
sudo ./test_validate

# v4l2-ctl / media-ctl commands
media-ctl -d /dev/media0 -p                                            # topology
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes                 # sensor formats
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f  # frame size range
v4l2-ctl -d /dev/v4l-subdev0 --set-subdev-fmt pad=0,width=1280,height=720  # change sensor
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5                # should fail: -EPIPE

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod vsoc_bridge
sudo rmmod vsoc_csi2
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Pipeline Validation Test (Part 10) ===

========================================
TEST 1: Matching formats (1920x1080)
========================================

  [sensor] pad=0 format set to 1920x1080 code=0x300f
  [csi2] pad=0 format set to 1920x1080 code=0x300f
  [video] format set to 1920x1080

  --- Test 1: attempting to stream ---
  VIDIOC_STREAMON SUCCESS

========================================
TEST 2: Mismatched formats
  sensor=1280x720, csi2=1920x1080
========================================

  [sensor] pad=0 format set to 1280x720 code=0x300f

  --- Test 2: attempting to stream ---
  VIDIOC_STREAMON FAILED: Broken pipe (errno=32)
  -> Pipeline validation error (format mismatch)

========================================
RESULTS SUMMARY
========================================

  Test 1 (matching formats):    PASS (streaming succeeded)
  Test 2 (mismatched formats):  PASS (streaming correctly refused)

All pipeline validation tests PASSED.
```

## What's New vs Previous Part

- Bridge embeds `struct media_pipeline pipe`
- `media_pipeline_start()` called in `start_streaming` before enabling any hardware
- `media_pipeline_stop()` called in `stop_streaming` after stopping hardware
- Video node entity gets `.link_validate` callback via `media_entity_operations`
- CSI-2 pad ops add `.link_validate = v4l2_subdev_link_validate_default`
- Format mismatches now return `-EPIPE` instead of silently corrupting output
- New test deliberately creates mismatched formats to verify validation

## Presentation Reference

Slides 43-48 of the V4L2 Subsystem Training deck (Pipeline Validation, media_pipeline_start, link_validate).
