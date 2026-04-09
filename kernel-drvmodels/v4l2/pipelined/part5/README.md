# Part 5: Subdev Controls

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces `v4l2_ctrl_handler` -- the V4L2 control framework that provides a standardized way to expose tunable parameters (exposure, gain, flip) to userspace. Controls are defined with type, range, step, and default value. The `s_ctrl` callback writes control values to sensor I2C registers. The `v4l2_ctrl_handler_setup()` call applies all default values to hardware at probe time. Userspace accesses controls through standard V4L2 ioctls (`VIDIOC_QUERYCTRL`, `VIDIOC_G_CTRL`, `VIDIOC_S_CTRL`).

## What You Build

The sensor driver (`vsoc_sensor.ko`) now includes a `v4l2_ctrl_handler` with 5 image controls: Exposure (0-65535), Analogue Gain (0-255), Digital Gain (0-255), Horizontal Flip (0/1), and Vertical Flip (0/1). Each control's `s_ctrl` callback writes the value to the corresponding sensor I2C register. The test program enumerates controls and verifies get/set roundtrip through `/dev/v4l-subdevN`.

## Key APIs

- `v4l2_ctrl_handler_init()` -- initialize a control handler
- `v4l2_ctrl_new_std()` -- create a standard V4L2 control
- `v4l2_ctrl_handler_setup()` -- apply all default control values to hardware
- `v4l2_ctrl_handler_free()` -- free control handler resources
- `.s_ctrl()` -- callback invoked when userspace sets a control value

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | Sensor driver with v4l2_ctrl_handler: exposure, gain, dgain, hflip, vflip |
| test_controls.c | Userspace test: enumerates controls, tests get/set for exposure, gain, hflip |
| Makefile | Builds vsoc_sensor.ko and test_controls binary |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko

# Test
sudo ./test_controls

# v4l2-ctl commands (alternative to test program)
v4l2-ctl -d /dev/v4l-subdev0 -L                               # list all controls
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl exposure=5000         # set exposure
v4l2-ctl -d /dev/v4l-subdev0 --get-ctrl exposure              # read back
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes         # formats (from Part 4)
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f  # frame sizes

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Part 5: Subdev Controls Test ===
Found vsoc_sensor at /dev/v4l-subdev0

=== Enumerating Controls ===
  [0x00980911] Exposure              min=0      max=65535  step=1   default=1000
  [0x009e0903] Analogue Gain         min=0      max=255    step=1   default=64
  [0x009e0905] Digital Gain          min=0      max=255    step=1   default=64
  [0x00980914] Horizontal Flip       min=0      max=1      step=1   default=0
  [0x00980915] Vertical Flip         min=0      max=1      step=1   default=0
  Total: 5 controls

=== Test Exposure Control ===
  Current exposure: 1000
  Setting exposure to 5000...
  Read back exposure: 5000
  PASS: exposure set correctly

=== Test Analogue Gain Control ===
  Current gain: 64
  Setting gain to 128...
  Read back gain: 128
  PASS: gain set correctly

=== Test Horizontal Flip Control ===
  Current hflip: 0
  Toggling hflip to 1...
  Read back hflip: 1
  PASS: hflip toggled correctly

=== Test PASSED ===
```

## What's New vs Previous Part

- Added `v4l2_ctrl_handler` with 5 controls (exposure, analogue gain, digital gain, hflip, vflip)
- Added `v4l2_ctrl_ops` with `.s_ctrl` callback that writes to sensor I2C registers
- Added `v4l2_ctrl_handler_setup()` to apply defaults at probe time
- Proper cleanup ordering in probe error path (ctrl handler freed before entity)
- `sd.ctrl_handler` pointer set to enable control inheritance by bridge drivers

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering V4L2 control framework, `v4l2_ctrl_handler`, standard controls (CID_EXPOSURE, CID_GAIN, CID_HFLIP, CID_VFLIP), and control inheritance.
