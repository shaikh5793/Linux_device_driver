# Part 11: Multi-Subdev Pipeline

Copyright (c) 2024 TECH VEDA. Author: Raghu Bharadwaj.

## Concept

This part introduces the **ISP (Image Signal Processor) subdev**, completing the full 4-entity SoC camera pipeline: Sensor -> CSI-2 -> ISP -> DMA. The ISP performs format conversion (Bayer SRGGB10 on its sink pad to RGB888 on its source pad) and is modeled as `MEDIA_ENT_F_PROC_VIDEO_ISP`. It has separate sink/source formats, sink-to-source dimension propagation, custom `link_validate`, and uses `v4l2_subdev_init_finalize()` for active state management.

## What You Build

- **vsoc_isp.ko** -- NEW: ISP platform subdev with 2 pads, Bayer-to-RGB format conversion, per-pad formats, link validation, `init_state` callback.
- **vsoc_bridge.ko** -- Bridge now finds and registers the ISP subdev, creates 4-entity link chain, streams ISP/CSI-2/sensor in order, uses RGB24 output format.
- **vsoc_csi2.ko** -- Same as Part 10.
- **vsoc_sensor.ko** -- Same as Part 10.
- **test_pipeline** -- Enumerates 4 entities and 3 links, captures 5 RGB24 frames, prints frame data.

## Key APIs

- `v4l2_subdev_init_finalize()` -- finalize subdev state initialization
- `v4l2_subdev_state_get_format()` -- get format from subdev active state
- `MEDIA_ENT_F_PROC_VIDEO_ISP` -- ISP entity function type
- `v4l2_subdev_cleanup()` -- clean up subdev state on remove

## Files

| File | Description |
|------|-------------|
| vsoc_bridge.c | Bridge with 4-entity pipeline (sensor->csi2->isp->video), RGB24 output |
| vsoc_sensor.c | Sensor subdev (same as Part 10) |
| vsoc_csi2.c | CSI-2 receiver (same as Part 10) |
| vsoc_isp.c | ISP subdev: SRGGB10 sink -> RGB888 source, link_validate, init_state |
| test_pipeline.c | Enumerate topology, capture 5 frames, print data bytes |
| Makefile | Builds vsoc_sensor.ko, vsoc_csi2.ko, vsoc_isp.ko, vsoc_bridge.ko, test_pipeline |

## Build & Run

```bash
# Build (hw module must be built first)
make -C ../hw
make

# Load
sudo insmod ../hw/soc_hw_platform.ko
sudo insmod vsoc_csi2.ko
sudo insmod vsoc_isp.ko
sudo insmod vsoc_bridge.ko
sudo insmod vsoc_sensor.ko    # loaded last — triggers async binding

# Test
sudo ./test_pipeline

# v4l2-ctl / media-ctl commands
media-ctl -d /dev/media0 -p                                            # 4-entity topology
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes                 # sensor formats
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f  # frame size range
v4l2-ctl -d /dev/video0 --all                                         # note: RGB24 output
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5                # capture 5 frames

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod vsoc_bridge
sudo rmmod vsoc_isp
sudo rmmod vsoc_csi2
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Multi-Subdev Pipeline Test (Part 11) ===

--- Media Topology ---
  Entity 1: 'vsoc_sensor' (type=0x00020001, pads=1, links=1)
  Entity 2: 'vsoc_csi2' (type=0x00020002, pads=2, links=2)
  Entity 3: 'vsoc_isp' (type=0x00020004, pads=2, links=2)
  Entity 4: 'VSOC-3000 Capture' (type=0x00020003, pads=1, links=1)
  Total entities: 4

--- Media Links ---
  [1]:0 -> [2]:0 [ENABLED] [IMMUTABLE]
  [2]:1 -> [3]:0 [ENABLED] [IMMUTABLE]
  [3]:1 -> [4]:0 [ENABLED] [IMMUTABLE]

Format set: 1920x1080, pixfmt=0x33424752, sizeimage=6220800
Streaming started

Frame 0: sequence=0, bytesused=6220800, data=[00 00 00 00 00 00]
Frame 1: sequence=1, bytesused=6220800, data=[01 00 00 00 00 00]
...

Full 4-entity pipeline test complete
```

## What's New vs Previous Part

- New module: `vsoc_isp.ko` with Bayer-to-RGB format conversion
- ISP has separate `sink_fmt` (SRGGB10) and `src_fmt` (RGB888)
- ISP sink pad propagates dimensions to source pad
- ISP uses `v4l2_subdev_init_finalize()` and `init_state` internal op
- Bridge creates 3-link chain: sensor->csi2->isp->video
- Bridge output format changes from SRGGB10 to RGB24
- Bridge `start_streaming` calls `s_stream` on ISP, CSI-2, then sensor
- Test verifies 4 entities in topology and captures real frame data

## Presentation Reference

Slides 49-55 of the V4L2 Subsystem Training deck (ISP Subdev, Multi-Entity Pipeline, Format Conversion).
