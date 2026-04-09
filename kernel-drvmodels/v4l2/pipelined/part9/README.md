# Part 9: Media Links & Topology

Copyright (c) 2024 TECH VEDA. Author: Raghu Bharadwaj.

## Concept

This part introduces **media pad links** and a new **CSI-2 receiver subdev**. The bridge creates explicit `media_create_pad_link()` connections between entities, forming a visible pipeline topology: `sensor:0 -> csi2:0, csi2:1 -> video:0`. The CSI-2 is an SoC-internal subdev registered directly (not via async) using `v4l2_device_register_subdev()`. The video node gets a sink pad. Userspace can now enumerate the full topology including entities, pads, and links via `MEDIA_IOC_ENUM_LINKS`.

## What You Build

- **vsoc_csi2.ko** -- NEW: CSI-2 receiver platform subdev with 2 pads (sink + source), `MEDIA_ENT_F_VID_IF_BRIDGE` function, format passthrough, MMIO register access.
- **vsoc_bridge.ko** -- Bridge now finds CSI-2 via platform device, registers it as a subdev, creates pad links, and adds a sink pad to the video node.
- **vsoc_sensor.ko** -- Same as Part 8 (1 source pad, CAM_SENSOR).
- **test_topology** -- Enumerates entities, pads, and links; prints full topology.

## Key APIs

- `media_create_pad_link()` -- create a link between two entity pads
- `v4l2_device_register_subdev()` -- register SoC-internal subdev (non-async)
- `media_entity_pads_init()` -- initialize pads on video node entity
- `MEDIA_IOC_ENUM_LINKS` -- enumerate links and pads from userspace

## Files

| File | Description |
|------|-------------|
| vsoc_bridge.c | Bridge with CSI-2 lookup, pad links, video node sink pad |
| vsoc_sensor.c | Sensor subdev (same as Part 8) |
| vsoc_csi2.c | CSI-2 receiver: 2 pads, format passthrough, MMIO registers |
| test_topology.c | Userspace test: enumerate entities, pads, and links |
| Makefile | Builds vsoc_sensor.ko, vsoc_csi2.ko, vsoc_bridge.ko, test_topology |

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
sudo ./test_topology

# v4l2-ctl / media-ctl commands
media-ctl -d /dev/media0 -p                                            # full topology
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes                 # sensor formats
v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f  # frame size range
v4l2-ctl -d /dev/v4l-subdev0 --get-subdev-fmt pad=0                   # current sensor format

# Cleanup
sudo rmmod vsoc_sensor
sudo rmmod vsoc_bridge
sudo rmmod vsoc_csi2
sudo rmmod soc_hw_platform
```

## Expected Output

```
=== VSOC-3000 Media Topology Test (Part 9) ===

Media Device: VSOC-3000 Camera

--- Entities ---

  Entity 1: "vsoc_sensor"
    Type:  CAM_SENSOR
    Pads:  1
    Links: 1

  Entity 2: "vsoc_csi2"
    Type:  VID_IF_BRIDGE
    Pads:  2
    Links: 2

  Entity 3: "VSOC-3000 Capture"
    Type:  IO_V4L
    Pads:  1
    Links: 1

Total: 3 entities

--- Links ---

  Entity 1 "vsoc_sensor":
    Pad 0: SOURCE
    Link: [1:0] -> [2:0] [ENABLED|IMMUTABLE]

  Entity 2 "vsoc_csi2":
    Pad 0: SINK
    Pad 1: SOURCE
    Link: [2:1] -> [3:0] [ENABLED|IMMUTABLE]
```

## What's New vs Previous Part

- New module: `vsoc_csi2.ko` (CSI-2 receiver with 2 pads, MMIO register access)
- Bridge finds CSI-2 subdev via `vsoc_hw_get_csi2_pdev()` + `platform_get_drvdata()`
- CSI-2 registered with `v4l2_device_register_subdev()` (SoC-internal, not async)
- Video node gets a sink pad (`MEDIA_PAD_FL_SINK`)
- `media_create_pad_link()` creates immutable enabled links
- Bridge `start_streaming` calls `s_stream` on CSI-2 before sensor
- New test enumerates pads and links (not just entities)

## Presentation Reference

Slides 36-42 of the V4L2 Subsystem Training deck (Media Links, CSI-2 Subdev, Pipeline Topology).
