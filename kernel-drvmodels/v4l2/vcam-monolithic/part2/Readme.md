# Part 2: Minimal V4L2 Capture Device -- Format Negotiation

Registers a V4L2 capture device backed by the VCAM-2000 hardware platform.
The driver reads the CHIP_ID register to verify hardware presence, implements
format negotiation ioctls, and writes format parameters to hardware registers.
No streaming, no VB2 -- this part is purely about device registration and
format negotiation.

**Source**: `vcam.c` | **Module**: `vcam.ko`

**Datasheet**: `../hw/VCAM-2000-Datasheet.md`

## Concepts Introduced

- **V4L2 device registration** -- `v4l2_device_register()` + `video_register_device()` creates `/dev/videoN`
- **Format negotiation ioctls** -- `ENUM_FMT`, `G/S/TRY_FMT`, `ENUM_FRAMESIZES`
- **Hardware register access** -- `ioread32()` / `iowrite32()` via `vcam_hw_map_regs()`
- **Chip ID verification** -- reading `VCAM_CHIP_ID` register at probe time
- **Writing format to hardware** -- `S_FMT` writes width, height, pixel format to hardware registers

## NOT Yet Covered (see Part 3)

- VB2 (videobuf2) streaming and buffer management
- Descriptor ring for frame delivery
- Interrupts and IRQ handling
- V4L2 controls framework

## Hardware Access Pattern

All hardware access uses `ioread32`/`iowrite32` on the register mapping
provided by `vcam_hw_platform.ko`:

```c
/* Map registers from hardware platform */
regs = vcam_hw_map_regs();

/* Read chip ID to verify hardware */
u32 chip = ioread32(regs + VCAM_CHIP_ID);

/* Write format parameters to hardware */
iowrite32(width,  regs + VCAM_FMT_WIDTH);
iowrite32(height, regs + VCAM_FMT_HEIGHT);
```

Register offsets are defined in `../hw/vcam_hw_interface.h`.

## Files

| File | Purpose |
|------|---------|
| `vcam.c` | Kernel module: minimal V4L2 capture device |
| `test_vcam.c` | Userspace: query caps, formats, set resolution |
| `Makefile` | kbuild module + clean |
| `run-demo.sh` | Automated build, load, test, cleanup |

## Driver Model

The driver uses the proper Linux platform bus binding model:

1. `vcam_hw_platform.ko` creates a platform device named `"vcam_hw"`
2. `vcam.ko` registers a `platform_driver` with `.driver.name = "vcam_hw"`
3. The platform bus matches by name and calls `probe()` automatically
4. The driver **never** creates its own platform device

This is the same model used by real hardware drivers, where devices come from
Device Tree, ACPI, or PCI enumeration -- the driver just declares what it can
handle and waits for the bus to hand it a matching device.

## Build and Run

```bash
make
sudo insmod ../hw/vcam_hw_platform.ko   # creates "vcam_hw" platform device
sudo insmod vcam.ko                     # bus matches -> probe() called
gcc -Wall -o test_vcam test_vcam.c
sudo ./test_vcam
sudo rmmod vcam                         # driver first
sudo rmmod vcam_hw_platform             # hardware last
```

Or use `run-demo.sh` for a guided walkthrough.

## Key V4L2 Concepts

- **Platform bus binding** -- driver registers with matching name, bus calls probe() when device is present
- **v4l2_device** -- represents the hardware (or virtual) device
- **video_device** -- the `/dev/videoN` node with file and ioctl operations
- **device_caps** -- tells userspace what this device supports
- **Format negotiation** -- `TRY_FMT` validates, `S_FMT` commits, driver clamps to supported range

## What to Explore Next

Part 3 adds VB2 (videobuf2) streaming with a hardware descriptor ring, IRQ
handler, and ring-based buffer management -- the device will generate frames
at 30 fps.
