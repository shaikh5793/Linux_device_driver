# VCAM-2000 Virtual Image Sensor Controller -- Basic Datasheet
## Part 2: V4L2 Device Skeleton

Copyright (c) 2024 TECH VEDA / Author: Raghu Bharadwaj

---

## 1. Overview

The VCAM-2000 is a virtual image sensor controller used throughout the V4L2
driver curriculum.  This datasheet covers **only** the hardware features
exercised by the Part 2 skeleton driver:

- Platform device enumeration (probe / remove)
- MMIO register access via `ioread32` / `iowrite32`
- CHIP_ID verification at probe time
- Format configuration registers (width, height, pixel format, stride, frame size)
- Basic enable / disable through the CTRL register
- STATUS register for readiness detection

Features such as descriptor rings, DMA, interrupts, and image controls
are **not** used in Part 2 and are documented in later datasheets.

### 1.1 Chip Identity

| Field             | Value        | Notes                           |
|-------------------|--------------|---------------------------------|
| CHIP_ID           | `0x00CA2000` | Read at probe to verify HW      |
| CHIP_REV          | `0x01`       | Revision number                  |
| Register File     | 128 x 32-bit | MMIO, 4-byte aligned            |

---

## 2. Controller Block Diagram

```
                  Platform Bus
                       |
         ==============|===============
         |    Platform Interface       |
         |  (register file decode)     |
         ==============|===============
                       |
            +----------+----------+
            |    Register File    |
            |   (512 bytes)       |
            |                     |
            |  CTRL      0x000   |
            |  STATUS    0x004   |
            |  INT_MASK  0x008   |  <-- not used in Part 2
            |  INT_STATUS 0x00C  |  <-- not used in Part 2
            |  CHIP_ID   0x010   |
            |  CHIP_REV  0x014   |
            |  FMT_WIDTH 0x020   |
            |  FMT_HEIGHT 0x024  |
            |  FMT_PIXFMT 0x028  |
            |  FMT_STRIDE 0x02C  |
            |  FMT_FRAMESIZE 0x030|
            +-----+---------+----+
                  |         |
           +------+--+ +---+------+
           | Control  | | Status   |
           | Logic    | | Logic    |
           |          | |          |
           | ENABLE --+--> READY   |
           |          | |          |
           +----------+ +----------+
                  |         |
            ======|=========|======
            |    Data Path         |  <-- idle in Part 2
            |  (sensor core,      |      (no streaming,
            |   DMA engine)       |       no interrupts)
            =======================
```

In Part 2 the data path is never activated.  The driver registers a V4L2
video device and implements format negotiation, but does not stream frames.

---

## 3. Register Map (Part 2 Subset)

All registers are 32-bit, little-endian, accessed through MMIO.

### 3.1 Core Control Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x000  | CTRL           | R/W    | 0x00000000   | Sensor control               |
| 0x004  | STATUS         | R      | 0x00000001   | Sensor status                |
| 0x010  | CHIP_ID        | R      | 0x00CA2000   | Chip identification          |
| 0x014  | CHIP_REV       | R      | 0x00000001   | Chip revision                |

INT_MASK (0x008) and INT_STATUS (0x00C) exist in the register file but
are not touched until Part 3 (interrupt handling).

### 3.2 Format Configuration Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x020  | FMT_WIDTH      | R/W    | 640          | Frame width in pixels        |
| 0x024  | FMT_HEIGHT     | R/W    | 480          | Frame height in lines        |
| 0x028  | FMT_PIXFMT     | R/W    | 0x33424752   | Pixel format (V4L2 fourcc)   |
| 0x02C  | FMT_STRIDE     | R/W    | 1920         | Bytes per line               |
| 0x030  | FMT_FRAMESIZE  | R/W    | 921600       | Total frame size in bytes    |

---

## 4. CTRL Register (0x000) Bit Fields

```
 31                              16 15                    4  1  0
+----------------------------------+----------------------+--+--+
|            Reserved (0)          |     Reserved (0)     |..|EN|
+----------------------------------+----------------------+--+--+
                                                              |
                                                              +-- Bit 0: ENABLE
```

### 4.1 Full Bit Definitions

```
  Bit    Name           Description
  ---------------------------------------------------------
   0     ENABLE         Power on sensor
   1     STREAM_ON      Start frame capture (Part 3+)
   4     RESET          Software reset, self-clearing (Part 3+)
   5     RING_ENABLE    Enable descriptor ring mode (Part 3+)
```

### 4.2 Part 2 Usage

| Bit | Name        | Part 2 role                                         |
|-----|-------------|-----------------------------------------------------|
|  0  | ENABLE      | Set to 1 at probe/open, cleared to 0 at stop/remove |
|  1  | STREAM_ON   | Not used until Part 3                                |
|  4  | RESET       | Not used until Part 3                                |
|  5  | RING_ENABLE | Not used until Part 3                                |

In Part 2, the driver writes **only** `ENABLE` (0x01) to power on the
sensor and `0x00` to power it off.

---

## 5. STATUS Register (0x004) Bit Fields

```
 31                              16 15                    2  1  0
+----------------------------------+----------------------+--+--+
|            Reserved (0)          |     Reserved (0)     |ST|RD|
+----------------------------------+----------------------+--+--+
                                                           |  |
                                                           |  +-- Bit 0: READY
                                                           +----- Bit 1: STREAMING
```

| Bit | Name      | Description                                       |
|-----|-----------|---------------------------------------------------|
|  0  | READY     | Sensor initialized and ready for configuration     |
|  1  | STREAMING | Currently capturing frames (Part 3+)               |

### Part 2 Usage

The driver reads STATUS at probe time.  If `READY` (bit 0) is set, the
sensor has been initialized by the platform module and is ready for
register access.

---

## 6. CHIP_ID Verification

The CHIP_ID register at offset 0x010 returns a fixed value of `0x00CA2000`.
The driver reads this register at probe time to verify it is talking to
real VCAM-2000 hardware.

```
  probe():
      id = ioread32(regs + 0x010)
      if (id != 0x00CA2000)
          return -ENODEV      // wrong hardware
      dev_info: "VCAM-2000 detected, rev %d"
```

---

## 7. Format Configuration

The format registers describe the image format the sensor will produce.
In Part 2, the driver programs these registers during `s_fmt` (set format)
but does not yet stream frames.

### 7.1 Supported Format

The VCAM-2000 supports a single pixel format:

| Property       | Value         | Notes                            |
|----------------|---------------|----------------------------------|
| Pixel Format   | `RGB24`       | V4L2 fourcc: 0x33424752 ("RGB3") |
| Bytes Per Pixel| 3             | One byte each for R, G, B        |
| Default Size   | 640 x 480     |                                  |
| Min Size       | 160 x 120     |                                  |
| Max Size       | 1920 x 1080   |                                  |
| Step           | 16            | Width/height must be aligned     |

### 7.2 Format Register Relationships

```
  FMT_STRIDE    = FMT_WIDTH * bytes_per_pixel
  FMT_FRAMESIZE = FMT_STRIDE * FMT_HEIGHT

  Example (default):
    FMT_WIDTH     = 640
    FMT_HEIGHT    = 480
    FMT_STRIDE    = 640 * 3 = 1920
    FMT_FRAMESIZE = 1920 * 480 = 921600
```

### 7.3 Programming Format Registers

```
  set_format(width, height):
      iowrite32(width,                    regs + 0x020)  // FMT_WIDTH
      iowrite32(height,                   regs + 0x024)  // FMT_HEIGHT
      iowrite32(0x33424752,               regs + 0x028)  // FMT_PIXFMT (RGB24)
      iowrite32(width * 3,                regs + 0x02C)  // FMT_STRIDE
      iowrite32(width * 3 * height,       regs + 0x030)  // FMT_FRAMESIZE
```

---

## 8. Probe / Remove Lifecycle

```
  insmod vcam_hw_platform.ko
       |
       v
  Platform module:
    - Allocates register file (128 x 32-bit)
    - Sets power-on defaults (CHIP_ID, format regs, etc.)
    - Creates platform device "vcam_hw"

  insmod vcam_part2.ko
       |
       v
  platform_driver_register()
       |
       v
  vcam_probe(pdev)
       |
       +-- vcam_hw_map_regs()        --> get __iomem pointer
       |
       +-- ioread32(CHIP_ID)         --> verify 0x00CA2000
       |
       +-- iowrite32(ENABLE, CTRL)   --> power on sensor
       |
       +-- v4l2_device_register()
       |
       +-- video_register_device()   --> creates /dev/videoN
       |
       v
  [device registered, format queries work]


  rmmod vcam_part2.ko
       |
       v
  vcam_remove(pdev)
       |
       +-- video_unregister_device()
       +-- v4l2_device_unregister()
       +-- iowrite32(0, CTRL)        --> power off sensor
       +-- vcam_hw_unmap_regs()
```

---

## 9. Programming Model

Part 2 uses **direct MMIO** via `ioread32` / `iowrite32` on the register
mapping returned by `vcam_hw_map_regs()`.

### 9.1 Reading a Register

```c
u32 chip_id;

chip_id = ioread32(priv->regs + VCAM_CHIP_ID);
if (chip_id != 0x00CA2000)
        return -ENODEV;
```

### 9.2 Writing a Register

```c
/* Enable the sensor (probe) */
iowrite32(VCAM_CTRL_ENABLE, priv->regs + VCAM_CTRL);

/* Disable the sensor (remove) */
iowrite32(0, priv->regs + VCAM_CTRL);
```

### 9.3 Reading Format Registers

```c
u32 width, height;

width  = ioread32(priv->regs + VCAM_FMT_WIDTH);
height = ioread32(priv->regs + VCAM_FMT_HEIGHT);
```

---

## 10. Platform Interface (Part 2)

The Part 2 driver uses two functions from the platform layer:

### 10.1 vcam_hw_map_regs

```
vcam_hw_map_regs()
  Returns: void __iomem *   (kernel virtual address of register file)
           NULL on failure

  Provides an MMIO pointer for ioread32/iowrite32 access to the
  128-register VCAM-2000 register file.
```

### 10.2 vcam_hw_unmap_regs

```
vcam_hw_unmap_regs()
  Returns: void

  Releases the register mapping obtained from vcam_hw_map_regs().
```

No other platform functions (IRQ, ring, notify) are needed in Part 2.

---

## 11. Hardware Constants

| Constant          | Value      | Description                |
|-------------------|------------|----------------------------|
| CHIP_ID           | 0x00CA2000 | Chip identification        |
| CHIP_REV          | 0x01       | Revision number            |
| REG_COUNT         | 128        | Number of 32-bit registers |
| DEF_WIDTH         | 640        | Default frame width        |
| DEF_HEIGHT        | 480        | Default frame height       |
| MIN_WIDTH         | 160        | Minimum frame width        |
| MAX_WIDTH         | 1920       | Maximum frame width        |
| MIN_HEIGHT        | 120        | Minimum frame height       |
| MAX_HEIGHT        | 1080       | Maximum frame height       |
| STEP              | 16         | Width/height alignment     |
| FMT_RGB24         | 0x33424752 | RGB24 pixel format code    |

---

## Appendix A: Quick Reference Card

```
CHIP_ID:    0x00CA2000
Register File: 128 x 32-bit MMIO registers

Key Registers for Part 2:
  CTRL         (0x000, R/W)  -- bit 0 = ENABLE
  STATUS       (0x004, R)    -- bit 0 = READY
  CHIP_ID      (0x010, R)    -- 0x00CA2000
  FMT_WIDTH    (0x020, R/W)  -- default 640
  FMT_HEIGHT   (0x024, R/W)  -- default 480
  FMT_PIXFMT   (0x028, R/W)  -- 0x33424752 (RGB24)
  FMT_STRIDE   (0x02C, R/W)  -- default 1920
  FMT_FRAMESIZE (0x030, R/W) -- default 921600

Probe sequence:
  vcam_hw_map_regs -> verify CHIP_ID -> enable sensor
  -> v4l2_device_register -> video_register_device

Remove sequence:
  video_unregister_device -> v4l2_device_unregister
  -> disable sensor -> vcam_hw_unmap_regs

Format set:
  iowrite32(width, FMT_WIDTH)
  iowrite32(height, FMT_HEIGHT)
  iowrite32(fourcc, FMT_PIXFMT)
  iowrite32(stride, FMT_STRIDE)
  iowrite32(framesize, FMT_FRAMESIZE)
```

---

*VCAM-2000 Basic Datasheet -- Part 2: V4L2 Device Skeleton*
*For use with the V4L2 Camera Driver Curriculum*
