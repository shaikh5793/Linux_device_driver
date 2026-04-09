# Part 7: Async Subdev Binding

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces `v4l2_async_notifier` -- the V4L2 asynchronous sub-device matching framework. In real hardware, sensor and bridge drivers may probe at unpredictable times (DT enumeration, deferred probe, hot-plug). The async framework decouples them: the bridge registers a notifier that watches for a sensor at a specific I2C bus+address, and the sensor calls `v4l2_async_register_subdev()` to announce itself. When both sides are present, the notifier fires `.bound` and `.complete` callbacks. This enables load-order independence -- sensor before bridge or bridge before sensor both work.

## What You Build

Two kernel modules: (1) the sensor driver (`vsoc_sensor.ko`) which calls `v4l2_async_register_subdev()` instead of waiting for direct discovery, and (2) the bridge driver (`vsoc_bridge.ko`) which uses `v4l2_async_nf_add_i2c()` to match the sensor by I2C adapter ID and address 0x10. The bridge defers `video_device` registration to the `.complete` callback, ensuring the video node only appears when the full pipeline is ready. All capture functionality from Part 6 (VB2 queue, DMA, IRQ) is preserved.

## Key APIs

- `v4l2_async_register_subdev()` -- sensor announces itself to the async framework
- `v4l2_async_unregister_subdev()` -- sensor deregisters from async framework
- `v4l2_async_nf_init()` -- initialize an async notifier on the bridge
- `v4l2_async_nf_add_i2c()` -- add an I2C match entry (bus ID + address)
- `v4l2_async_nf_register()` -- activate the notifier
- `.bound()` -- callback when a matching subdev is found
- `.complete()` -- callback when all expected subdevs are bound
- `.unbind()` -- callback when a subdev is removed

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | Sensor driver using v4l2_async_register_subdev() for async binding |
| vsoc_bridge.c | Bridge driver with v4l2_async_notifier, deferred video_device registration |
| test_async.c | Userspace test: prints load-order test instructions, checks for /dev/videoN |
| Makefile | Builds vsoc_sensor.ko, vsoc_bridge.ko, and test_async; includes install-reverse target |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load — Recommended order: sensor last (triggers async binding)
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_bridge.ko
sudo insmod vsoc_sensor.ko    # loaded last — triggers async binding

# Test
sudo ./test_async

# Cleanup
sudo rmmod vsoc_bridge
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform

# Load — Alternate order: sensor first (demonstrates load-order independence)
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko
sudo insmod vsoc_bridge.ko

# Verify: /dev/video0 should still appear
sudo ./test_async

# Cleanup
sudo rmmod vsoc_bridge
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
# dmesg (Test A — sensor first):
vsoc_sensor 0-0010: VSOC-3000 sensor detected (ID: 0x3000)
vsoc_sensor 0-0010: v4l2_subdev 'vsoc_sensor' async-registered (waiting for bridge)
vsoc_bridge vsoc_bridge: VSOC-3000 bridge probed (waiting for async sensor match)
vsoc_bridge vsoc_bridge: sensor subdev 'vsoc_sensor' bound
vsoc_bridge vsoc_bridge: VSOC-3000 bridge registered as /dev/video0 (async complete)

# dmesg (Test B — bridge first):
vsoc_bridge vsoc_bridge: VSOC-3000 bridge probed (waiting for async sensor match)
vsoc_sensor 0-0010: VSOC-3000 sensor detected (ID: 0x3000)
vsoc_sensor 0-0010: v4l2_subdev 'vsoc_sensor' async-registered (waiting for bridge)
vsoc_bridge vsoc_bridge: sensor subdev 'vsoc_sensor' bound
vsoc_bridge vsoc_bridge: VSOC-3000 bridge registered as /dev/video0 (async complete)

# test_async output:
=== VSOC-3000 Part 7: Async Subdev Binding Test ===
...
CHECKING CURRENT STATE:
  RESULT: /dev/video0 found!
    Driver: vsoc_bridge
    Card:   VSOC-3000 Bridge (async)
    Caps:   0x05200003
    CAPTURE + STREAMING: OK

  Both modules appear to be loaded and bound.
  Try running: ../part5/test_capture
```

## What's New vs Previous Part

- Sensor uses `v4l2_async_register_subdev()` instead of passive discovery
- Bridge uses `v4l2_async_notifier` with `v4l2_async_nf_add_i2c()` to match sensor
- Notifier callbacks: `.bound` (control inheritance), `.complete` (video_device registration), `.unbind` (cleanup)
- `video_device` registration deferred to `.complete` -- video node only appears when pipeline is ready
- `video_registered` flag guards teardown in both `.unbind` and `remove`
- Load-order independence: sensor-first and bridge-first both work
- Bridge direct I2C bus lookup (`bus_find_device_by_name`) removed
- Makefile adds `install-reverse` target for bridge-first testing

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering V4L2 async framework, `v4l2_async_notifier`, `v4l2_async_connection`, I2C match criteria, notifier lifecycle (bound/complete/unbind), and load-order independence.
