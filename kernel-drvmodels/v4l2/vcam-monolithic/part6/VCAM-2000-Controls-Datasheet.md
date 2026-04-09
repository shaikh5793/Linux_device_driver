# VCAM-2000 Virtual Image Sensor Controller -- Controls Datasheet
## Part 4: V4L2 Image Controls

Copyright (c) 2024 TECH VEDA / Author: Raghu Bharadwaj

**Applicable to**: Part 4 (V4L2 Control Framework -- Brightness & Horizontal Flip)

This document describes the image control registers of the VCAM-2000 as used
by the Part 4 driver.  It covers only the hardware features added in Part 4;
for register basics and streaming, see the Part 2 and Part 3 datasheets.

---

## 1. Overview

The VCAM-2000 image pipeline includes two configurable controls that affect
the generated frame data in real time:

- **Brightness** -- adjusts the luminance offset applied to each pixel
- **Horizontal Flip** -- mirrors the image horizontally

These controls are implemented as hardware registers that the sensor core
reads on every frame.  Changes take effect on the **next frame** after the
register write -- no restart or reconfiguration is needed.

```
  Sensor Core           Image Pipeline              DMA Engine
  (~30 fps)             (per-scanline)              (to buffer)
  +-----------+         +------------------+        +-----------+
  | Generate  |  --->   | Apply BRIGHTNESS |  --->  | Write to  |
  | 8-bar     |         | Apply HFLIP      |        | descriptor|
  | pattern   |         |                  |        | buffer    |
  +-----------+         +------------------+        +-----------+
                              ^       ^
                              |       |
                         BRIGHTNESS  HFLIP
                          (0x040)   (0x044)
                              |       |
                        Register File (MMIO)
```

---

## 2. Image Control Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x040  | BRIGHTNESS     | R/W    | 128          | Brightness offset (0--255)   |
| 0x044  | HFLIP          | R/W    | 0            | Horizontal flip (0 or 1)     |

---

## 3. BRIGHTNESS Register (0x040)

### 3.1 Register Format

```
 31                              16 15                    8 7             0
+----------------------------------+----------------------+---------------+
|            Reserved (0)          |     Reserved (0)     |  BRIGHTNESS   |
+----------------------------------+----------------------+---------------+
                                                          |<--- 8 bits -->|
```

Only bits 7:0 are used.  Bits 31:8 are reserved and read as zero.

### 3.2 Brightness Processing

The hardware interprets the register value as an unsigned 8-bit number and
converts it to a signed offset for pixel processing:

```
  signed_offset = BRIGHTNESS_REG - 128

  Register Value    Signed Offset    Effect
  -----------------------------------------------
       0               -128          Maximum darkening
      64                -64          Moderate darkening
     128                  0          No change (default)
     192                +64          Moderate brightening
     255               +127          Maximum brightening
```

### 3.3 Per-Pixel Application

For each pixel in the generated frame, the hardware applies brightness
as follows:

```
  For each color channel (R, G, B):
      adjusted = channel_value + signed_offset
      output   = clamp(adjusted, 0, 255)
```

The clamp operation ensures the result stays within the valid 8-bit range.

### 3.4 V4L2 Control Mapping

| Property     | Value     |
|--------------|-----------|
| V4L2 CID     | `V4L2_CID_BRIGHTNESS` |
| Type         | Integer   |
| Minimum      | 0         |
| Maximum      | 255       |
| Step         | 1         |
| Default      | 128       |

### 3.5 Programming Example

```c
/* Set brightness to maximum brightening */
iowrite32(255, priv->regs + VCAM_BRIGHTNESS);

/* Set brightness to default (no change) */
iowrite32(128, priv->regs + VCAM_BRIGHTNESS);

/* Set brightness to maximum darkening */
iowrite32(0, priv->regs + VCAM_BRIGHTNESS);
```

---

## 4. HFLIP Register (0x044)

### 4.1 Register Format

```
 31                              16 15                    1           0
+----------------------------------+----------------------+-----------+
|            Reserved (0)          |     Reserved (0)     |   HFLIP   |
+----------------------------------+----------------------+-----------+
                                                          |<- 1 bit ->|
```

Only bit 0 is used.  Bits 31:1 are reserved and read as zero.

### 4.2 Flip Behavior

| Value | Behavior                                                    |
|-------|-------------------------------------------------------------|
| 0     | Normal -- pixels generated left-to-right per scanline       |
| 1     | Mirror -- pixels generated right-to-left per scanline       |

When HFLIP is enabled, the hardware reads pixel positions in reverse
order when writing each scanline to the output buffer.  The 8-bar
color pattern appears mirrored:

```
  HFLIP = 0 (normal):
  +-------+-------+-------+-------+-------+-------+-------+-------+
  | White |Yellow | Cyan  | Green |Magenta|  Red  | Blue  | Black |
  +-------+-------+-------+-------+-------+-------+-------+-------+
   bar 0    bar 1   bar 2   bar 3   bar 4   bar 5   bar 6   bar 7

  HFLIP = 1 (mirrored):
  +-------+-------+-------+-------+-------+-------+-------+-------+
  | Black | Blue  |  Red  |Magenta| Green | Cyan  |Yellow | White |
  +-------+-------+-------+-------+-------+-------+-------+-------+
   bar 7    bar 6   bar 5   bar 4   bar 3   bar 2   bar 1   bar 0
```

### 4.3 V4L2 Control Mapping

| Property     | Value     |
|--------------|-----------|
| V4L2 CID     | `V4L2_CID_HFLIP` |
| Type         | Boolean   |
| Minimum      | 0         |
| Maximum      | 1         |
| Step         | 1         |
| Default      | 0         |

### 4.4 Programming Example

```c
/* Enable horizontal flip */
iowrite32(1, priv->regs + VCAM_HFLIP);

/* Disable horizontal flip (default) */
iowrite32(0, priv->regs + VCAM_HFLIP);
```

---

## 5. Real-Time Control Update

Both BRIGHTNESS and HFLIP are sampled by the hardware on every frame
generation cycle.  The update is glitch-free:

```
  Frame N generation begins
       |
       v
  Hardware reads BRIGHTNESS (0x040)     <-- uses current value
  Hardware reads HFLIP (0x044)          <-- uses current value
       |
       v
  Generate scanlines with current settings
       |
       v
  Frame N complete
       |
  [Driver writes new BRIGHTNESS value here]
       |
       v
  Frame N+1 generation begins
       |
       v
  Hardware reads BRIGHTNESS (0x040)     <-- uses NEW value
  Hardware reads HFLIP (0x044)
       |
       v
  Generate scanlines with new settings
```

There is no need to stop streaming, reconfigure, and restart.  The V4L2
control handler `s_ctrl` callback simply writes the register and the
change takes effect on the next frame.

### 5.1 Control Update Sequence

```
  User calls VIDIOC_S_CTRL(CID_BRIGHTNESS, value)
       |
       v
  V4L2 control framework validates range (0--255)
       |
       v
  Driver s_ctrl() callback:
       iowrite32(value, priv->regs + VCAM_BRIGHTNESS)
       |
       v
  [Next frame uses new brightness]
```

---

## 6. Combined Effect

Both controls are applied in sequence during frame generation:

```
  1. Generate base color for pixel (from 8-bar pattern)
  2. Apply BRIGHTNESS offset (add signed_offset, clamp 0--255)
  3. Apply HFLIP (reverse scanline pixel order if enabled)
  4. Write pixel to output buffer
```

The order matters: brightness adjusts color values first, then the
horizontal flip determines pixel placement in the buffer.

---

## 7. Hardware Constants

| Constant             | Value | Description                    |
|----------------------|-------|--------------------------------|
| BRIGHTNESS_DEFAULT   | 128   | No brightness adjustment       |
| BRIGHTNESS_MIN       | 0     | Maximum darkening              |
| BRIGHTNESS_MAX       | 255   | Maximum brightening            |
| HFLIP_DEFAULT        | 0     | Normal (no flip)               |
| HFLIP_ENABLED        | 1     | Horizontal mirror              |

---

## Appendix A: Quick Reference Card

```
Image Control Registers:
  BRIGHTNESS (0x040, R/W) -- 0..255, default 128
  HFLIP      (0x044, R/W) -- 0 or 1, default 0

Brightness processing:
  signed_offset = register_value - 128
  output_pixel  = clamp(base_pixel + signed_offset, 0, 255)

HFLIP processing:
  0 = left-to-right (normal)
  1 = right-to-left (mirrored)

V4L2 control IDs:
  V4L2_CID_BRIGHTNESS -- integer, min=0, max=255, default=128
  V4L2_CID_HFLIP      -- boolean, min=0, max=1, default=0

Real-time update:
  Write register -> next frame uses new value
  No streaming restart needed
```

---

*VCAM-2000 Controls Datasheet -- Part 4: V4L2 Image Controls*
*For use with the V4L2 Camera Driver Curriculum*
