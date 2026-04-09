# Part 6: Bridge-Owned Subdev with Video Capture

Copyright (c) 2024 TECH VEDA | Author: Raghu Bharadwaj

## Concept

This part introduces the bridge driver pattern -- the standard architecture for V4L2 camera systems. The bridge driver owns the `v4l2_device`, discovers the sensor subdev, registers it, creates a `video_device` (`/dev/videoN`), and manages a VB2 buffer queue for frame capture. The sensor no longer owns its own `v4l2_device`; instead, the bridge registers the sensor subdev into its media graph. Control inheritance allows the bridge's video node to expose the sensor's controls. A DMA descriptor ring and ISR/work-queue mechanism handle frame completion.

## What You Build

Two kernel modules: (1) the sensor driver (`vsoc_sensor.ko`) which initializes its subdev but does NOT register a `v4l2_device` -- it waits for the bridge, and (2) the bridge driver (`vsoc_bridge.ko`) which discovers the sensor via I2C bus lookup, registers it as a subdev, sets up VB2 queues (MMAP/READ/DMABUF), programs DMA registers, handles interrupts, and creates `/dev/videoN`. The test program performs a full capture cycle: QUERYCAP, S_FMT, REQBUFS, QBUF, STREAMON, DQBUF (5 frames), STREAMOFF.

## Key APIs

- `video_register_device()` -- register a V4L2 video device node
- `vb2_queue_init()` -- initialize a VB2 buffer queue
- `vb2_ops` -- queue_setup, buf_prepare, buf_queue, start/stop_streaming
- `v4l2_ctrl_add_handler()` -- inherit controls from sensor subdev
- `v4l2_subdev_call(sd, video, s_stream, 1)` -- start sensor from bridge
- `request_irq()` -- register DMA frame-done interrupt handler
- `vsoc_hw_set_buf_ring()` -- register descriptor ring with DMA engine
- `vsoc_hw_notify_stream()` -- notify hardware to start/stop frame generation

## Files

| File | Description |
|------|-------------|
| vsoc_sensor.c | Sensor driver (no v4l2_device); initializes subdev for bridge ownership |
| vsoc_bridge.c | Bridge driver: video_device, VB2 queue, DMA ring, IRQ, control inheritance |
| test_capture.c | Userspace test: full MMAP capture cycle (4 buffers, 5 frames at 640x480 RGB24) |
| Makefile | Builds vsoc_sensor.ko, vsoc_bridge.ko, and test_capture binary |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Recommended load order (sensor last — triggers async binding)
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_bridge.ko
sudo insmod vsoc_sensor.ko    # loaded last — triggers async binding

# Test
sudo ./test_capture

# Cleanup
sudo rmmod vsoc_bridge
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform

# Alternate load order (sensor first — also works due to async binding)
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_sensor.ko
sudo insmod vsoc_bridge.ko

# Test (same result — load-order independence)
sudo ./test_capture

# Cleanup
sudo rmmod vsoc_bridge
sudo rmmod vsoc_sensor
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Part 6: Capture Test ===

Opened /dev/video0 (fd=3)
Driver:   vsoc_bridge
Card:     VSOC-3000 Bridge
Bus:      platform:vsoc_bridge
Caps:     0x05200003
  VIDEO_CAPTURE: YES
  STREAMING:     YES

Format set: 640x480, pixfmt=RGB3, sizeimage=921600
Allocated 4 MMAP buffers
  Buffer 0: length=921600, mapped=0x...
  Buffer 1: length=921600, mapped=0x...
  Buffer 2: length=921600, mapped=0x...
  Buffer 3: length=921600, mapped=0x...

Streaming ON

Frame 0: seq=0, ts=..., bytes=921600, first4=[xx xx xx xx]
Frame 1: seq=1, ts=..., bytes=921600, first4=[xx xx xx xx]
Frame 2: seq=2, ts=..., bytes=921600, first4=[xx xx xx xx]
Frame 3: seq=3, ts=..., bytes=921600, first4=[xx xx xx xx]
Frame 4: seq=4, ts=..., bytes=921600, first4=[xx xx xx xx]

Streaming OFF

=== Test complete ===
```

## What's New vs Previous Part

- Sensor no longer registers its own `v4l2_device` (bridge-owned architecture)
- New bridge driver (`vsoc_bridge.c`) with full capture pipeline
- `video_device` registered as `/dev/videoN` with `VFL_DIR_RX`
- VB2 buffer queue with MMAP, READ, and DMABUF support
- DMA descriptor ring (`vsoc_hw_desc`) for hardware buffer management
- ISR + work queue for frame-done interrupt handling
- Control inheritance: bridge exposes sensor controls via `v4l2_ctrl_add_handler()`
- Added `V4L2_CID_GAIN` and `V4L2_CID_TEST_PATTERN` menu control (replaces CID_ANALOGUE_GAIN/CID_DIGITAL_GAIN from Part 5)
- Load order is fixed: sensor must load before bridge (resolved in Part 7)

## Presentation Reference

V4L2 Subsystem Training deck: Slides covering bridge driver architecture, `video_device` registration, VB2 buffer management, DMA descriptor rings, interrupt handling, and control inheritance.
