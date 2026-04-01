# VSOC-3000 Virtual SoC Camera Platform Datasheet

**Revision 1.0** | **Copyright (c) 2024 TECH VEDA** | **Author: Raghu Bharadwaj**

---

## 1. Device Overview

| Parameter              | Value                                            |
|------------------------|--------------------------------------------------|
| Device Name            | VSOC-3000 Virtual SoC Camera Platform            |
| Architecture           | 4-block camera pipeline                          |
| Pipeline               | Sensor (I2C) -> CSI-2 Receiver -> ISP -> DMA     |
| Platform Module        | `soc_hw_platform.ko`                             |
| Header File            | `soc_hw_interface.h`                             |
| License                | GPL-2.0                                          |
| Version                | 1.0.0                                            |

The VSOC-3000 is a virtual SoC camera pipeline with four independent
hardware blocks connected in series.  It models a real embedded camera
subsystem found in mobile and automotive SoCs.  Each block has its own
register space and is accessed independently by its corresponding
Linux kernel driver module.

The VSOC-3000 is entirely virtual --- it runs on any Linux system
without real camera hardware.  The platform module (`soc_hw_platform.ko`)
creates virtual I2C buses, MMIO register arrays, platform devices, and
a software interrupt, allowing driver modules to be developed and tested
using the same kernel APIs that target real hardware.

---

## 2. System Architecture

### 2.1 Block Diagram

```
  +-------------------------------------------------------------------+
  |                        VSOC-3000 SoC                              |
  |                                                                   |
  |  +-----------+     +-------------+     +-------+     +----------+ |
  |  |  Camera   | I2C |   CSI-2     | AXI |       | AXI |   DMA    | |
  |  |  Sensor   |---->|  Receiver   |---->|  ISP  |---->|  Engine  | |
  |  |  (0x10)   | bus |             |     |       |     |          | |
  |  +-----------+     +-------------+     +-------+     +----------+ |
  |       |                  |                 |              |        |
  |    16x16b             16x32b            16x32b        144x32b     |
  |    regs               regs              regs          regs        |
  |                                                       |   |       |
  |                                                  IRQ--+   |       |
  |                                                       DMA to     |
  |                                                     system mem   |
  +-------------------------------------------------------------------+
```

### 2.2 Data Flow

```
  Sensor (Bayer RAW 10-bit)
      |
      | MIPI CSI-2 (virtual)
      v
  CSI-2 Receiver (format passthrough)
      |
      | AXI internal bus (virtual)
      v
  ISP (Bayer -> RGB888 conversion, brightness/contrast)
      |
      | AXI internal bus (virtual)
      v
  DMA Engine (descriptor ring -> system memory)
      |
      | Software IRQ
      v
  Driver (buffer completion callback)
```

### 2.3 Bus Types

| Bus          | Usage                         | Width     |
|--------------|-------------------------------|-----------|
| I2C          | Sensor register access        | 16-bit    |
| MIPI CSI-2   | Sensor-to-CSI data transport  | Virtual   |
| AXI          | Internal block interconnect   | 32-bit    |
| System       | DMA to host memory            | 64-bit addr |

---

## 3. Camera Sensor Block (I2C)

### 3.1 Overview

| Parameter           | Value                                |
|---------------------|--------------------------------------|
| I2C Address         | `0x10`                               |
| Register Width      | 16-bit addresses, 16-bit values      |
| Register Count      | 16 registers                         |
| Access Protocol     | SMBus word read/write                |
| Chip ID             | `0x3000`                             |
| Chip Revision       | `0x01`                               |
| I2C Bus Name        | `vsoc-i2c`                           |
| Board Name          | `vsoc_sensor`                        |

### 3.2 Register Map

| Offset | Name                   | R/W | Default  | Description                     |
|--------|------------------------|-----|----------|---------------------------------|
| 0x00   | CHIP_ID                | RO  | 0x3000   | Chip identification             |
| 0x02   | CHIP_REV               | RO  | 0x01     | Chip revision                   |
| 0x04   | CTRL                   | RW  | 0x0000   | Control register                |
| 0x06   | WIDTH                  | RW  | 1920     | Active image width              |
| 0x08   | HEIGHT                 | RW  | 1080     | Active image height             |
| 0x0A   | FMT                    | RW  | 0x300F   | Media bus format (low 16 bits)  |
| 0x0C   | EXPOSURE               | RW  | 1000     | Exposure time (0-65535)         |
| 0x0E   | GAIN                   | RW  | 64       | Analog gain (0-255)             |
| 0x10   | DGAIN                  | RW  | 64       | Digital gain (0-255)            |
| 0x12   | HFLIP                  | RW  | 0        | Horizontal flip (0/1)           |
| 0x14   | VFLIP                  | RW  | 0        | Vertical flip (0/1)             |
| 0x16   | TESTPAT                | RW  | 0        | Test pattern (0=off, 1-4)       |
| 0x18   | STATUS                 | RO  | 0x0001   | Sensor status                   |

### 3.3 CTRL Register (0x04) Bit Definitions

| Bit | Name    | Description                              |
|-----|---------|------------------------------------------|
| 0   | ENABLE  | Power on the sensor (1 = enabled)        |
| 1   | STREAM  | Start streaming (1 = streaming)          |

### 3.4 STATUS Register (0x18) Bit Definitions

| Bit | Name      | Description                              |
|-----|-----------|------------------------------------------|
| 0   | READY     | Sensor is powered and ready              |
| 1   | STREAMING | Sensor is actively streaming             |

### 3.5 Resolution Constraints

| Parameter       | Value     |
|-----------------|-----------|
| Minimum Width   | 160       |
| Maximum Width   | 3840      |
| Minimum Height  | 120       |
| Maximum Height  | 2160      |
| Step            | 16        |
| Default         | 1920x1080 |

### 3.6 Supported Formats

| Format Code | Name     | Description               |
|-------------|----------|---------------------------|
| 0x300F      | SRGGB10  | Bayer RAW 10-bit (RGGB)   |
| (RGB888)    | RGB888   | 24-bit RGB                |

### 3.7 Control Ranges

| Control   | Min  | Max    | Default | Step |
|-----------|------|--------|---------|------|
| Exposure  | 0    | 65535  | 1000    | 1    |
| Gain      | 0    | 255    | 64      | 1    |
| DGain     | 0    | 255    | 64      | 1    |
| HFlip     | 0    | 1      | 0       | 1    |
| VFlip     | 0    | 1      | 0       | 1    |
| TestPat   | 0    | 4      | 0       | 1    |

### 3.8 Read-Only Register Protection

The following registers are read-only and ignore writes:

- `CHIP_ID` (0x00)
- `CHIP_REV` (0x02)
- `STATUS` (0x18)

### 3.9 Power-On Sequence

```
  1. Read CHIP_ID  -> verify 0x3000
  2. Read CHIP_REV -> verify 0x01
  3. Read STATUS   -> verify READY bit is set
  4. Write CTRL.ENABLE = 1
```

### 3.10 Streaming Start/Stop

```
  Start:
    1. Configure WIDTH, HEIGHT, FMT
    2. Set desired controls (EXPOSURE, GAIN, etc.)
    3. Write CTRL = ENABLE | STREAM

  Stop:
    1. Write CTRL = ENABLE (clear STREAM bit)
    2. Optionally clear ENABLE
```

---

## 4. CSI-2 Receiver Block

### 4.1 Overview

| Parameter           | Value                                |
|---------------------|--------------------------------------|
| Register Type       | MMIO, 32-bit                         |
| Register Count      | 16 registers                         |
| Access              | `ioread32()` / `iowrite32()`         |
| Platform Device     | `vsoc_csi2`                          |
| Default Lanes       | 2                                    |

### 4.2 Register Map

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x000  | CTRL                   | RW  | 0x00000000   | Control register                |
| 0x004  | STATUS                 | RO  | 0x00000001   | Status register                 |
| 0x008  | FMT                    | RW  | 0x00000000   | Media bus format code           |
| 0x00C  | WIDTH                  | RW  | 0x00000000   | Frame width                     |
| 0x010  | HEIGHT                 | RW  | 0x00000000   | Frame height                    |
| 0x014  | LANES                  | RW  | 0x00000002   | Number of data lanes (1-4)      |
| 0x018  | INT_MASK               | RW  | 0xFFFFFFFF   | Interrupt mask (1=disabled)     |
| 0x01C  | INT_STATUS             | RW  | 0x00000000   | Interrupt status (W1C)          |

### 4.3 CTRL Register (0x000) Bit Definitions

| Bit | Name    | Description                              |
|-----|---------|------------------------------------------|
| 0   | ENABLE  | Enable the CSI-2 receiver                |
| 1   | STREAM  | Start receiving data                     |

### 4.4 STATUS Register (0x004) Bit Definitions

| Bit | Name      | Description                              |
|-----|-----------|------------------------------------------|
| 0   | READY     | Receiver is powered and ready            |
| 1   | STREAMING | Receiver is actively receiving           |
| 2   | LINK_UP   | CSI-2 link is established                |

### 4.5 Interrupt Bits (INT_MASK / INT_STATUS)

| Bit | Name        | Description                              |
|-----|-------------|------------------------------------------|
| 0   | FRAME_START | Start of frame received                  |
| 1   | FRAME_END   | End of frame received                    |
| 2   | ERROR       | Receive error                            |

### 4.6 Lane Configuration

The CSI-2 receiver supports 1 to 4 data lanes.  The default is 2 lanes.
The lane count is written to the LANES register before enabling the
receiver.  Format, width, and height pass through from the sensor
configuration.

---

## 5. ISP Block

### 5.1 Overview

| Parameter           | Value                                |
|---------------------|--------------------------------------|
| Register Type       | MMIO, 32-bit                         |
| Register Count      | 16 registers                         |
| Access              | `ioread32()` / `iowrite32()`         |
| Platform Device     | `vsoc_isp`                           |
| Input Format        | SRGGB10 (Bayer RAW 10-bit)           |
| Output Format       | RGB888 (24-bit RGB)                  |

### 5.2 Register Map

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x000  | CTRL                   | RW  | 0x00000000   | Control register                |
| 0x004  | STATUS                 | RO  | 0x00000001   | Status register                 |
| 0x008  | IN_FMT                 | RW  | 0x00000000   | Input media bus format          |
| 0x00C  | OUT_FMT                | RW  | 0x00000000   | Output media bus format         |
| 0x010  | WIDTH                  | RW  | 0x00000000   | Frame width                     |
| 0x014  | HEIGHT                 | RW  | 0x00000000   | Frame height                    |
| 0x018  | BRIGHTNESS             | RW  | 128          | Brightness (0-255)              |
| 0x01C  | CONTRAST               | RW  | 128          | Contrast (0-255)                |
| 0x020  | INT_MASK               | RW  | 0xFFFFFFFF   | Interrupt mask (1=disabled)     |
| 0x024  | INT_STATUS             | RW  | 0x00000000   | Interrupt status (W1C)          |

### 5.3 CTRL Register (0x000) Bit Definitions

| Bit | Name    | Description                              |
|-----|---------|------------------------------------------|
| 0   | ENABLE  | Enable the ISP                           |
| 1   | STREAM  | Start processing frames                  |
| 2   | BYPASS  | Bypass all processing (passthrough)      |

### 5.4 STATUS Register (0x004) Bit Definitions

| Bit | Name      | Description                              |
|-----|-----------|------------------------------------------|
| 0   | READY     | ISP is powered and ready                 |
| 1   | STREAMING | ISP is actively processing               |
| 2   | BUSY      | ISP is processing a frame                |

### 5.5 Interrupt Bits (INT_MASK / INT_STATUS)

| Bit | Name        | Description                              |
|-----|-------------|------------------------------------------|
| 0   | FRAME_DONE  | Frame processing complete                |
| 1   | ERROR       | Processing error                         |

### 5.6 Processing Controls

| Control    | Offset | Range   | Default | Description                         |
|------------|--------|---------|---------|-------------------------------------|
| Brightness | 0x018  | 0-255   | 128     | Applied as offset: (value - 128)    |
| Contrast   | 0x01C  | 0-255   | 128     | Contrast adjustment                 |

Brightness is applied to each RGB component of the test pattern as an
additive offset: `component + (brightness - 128)`, clamped to [0, 255].

### 5.7 Bypass Mode

When `CTRL.BYPASS` (bit 2) is set, the ISP passes data through without
any format conversion or image processing.  Input format must match
output format when bypass is enabled.

---

## 6. DMA Engine Block

### 6.1 Overview

| Parameter           | Value                                |
|---------------------|--------------------------------------|
| Register Type       | MMIO, 32-bit                         |
| Register Count      | 144 registers                        |
| Access              | `ioread32()` / `iowrite32()`         |
| Platform Device     | `vsoc_bridge`                        |
| Output Pixel Format | RGB24 (`0x33424752`)                 |
| IRQ Type            | Software (dynamically allocated)     |

### 6.2 Register Map --- Control and Status

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x000  | CTRL                   | RW  | 0x00000000   | Control register                |
| 0x004  | STATUS                 | RO  | 0x00000001   | Status register                 |
| 0x008  | INT_MASK               | RW  | 0xFFFFFFFF   | Interrupt mask (1=disabled)     |
| 0x00C  | INT_STATUS             | RW  | 0x00000000   | Interrupt status (W1C)          |

### 6.3 Register Map --- Format Configuration

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x010  | FMT_WIDTH              | RW  | 1920         | Frame width in pixels           |
| 0x014  | FMT_HEIGHT             | RW  | 1080         | Frame height in pixels          |
| 0x018  | FMT_STRIDE             | RW  | 5760         | Bytes per line (width * 3)      |
| 0x01C  | FMT_FRAMESIZE          | RW  | 6220800      | Total frame size in bytes       |

The default frame size is `1920 * 3 * 1080 = 6,220,800` bytes (RGB24).

### 6.4 Register Map --- Buffer Ring

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x100  | BUF_RING_ADDR          | RW  | 0x00000000   | Ring base address (set by API)  |
| 0x104  | BUF_RING_SIZE          | RW  | 0x00000000   | Number of descriptors in ring   |
| 0x108  | BUF_RING_HEAD          | RW  | 0x00000000   | Head pointer (driver writes)    |
| 0x10C  | BUF_RING_TAIL          | RO  | 0x00000000   | Tail pointer (hardware writes)  |

### 6.5 Register Map --- Frame Metadata

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x110  | FRAME_COUNT            | RO  | 0x00000000   | Monotonic frame counter         |
| 0x114  | FRAME_TS_LO            | RO  | 0x00000000   | Timestamp low 32 bits (ns)      |
| 0x118  | FRAME_TS_HI            | RO  | 0x00000000   | Timestamp high 32 bits (ns)     |

### 6.6 Register Map --- Statistics

| Offset | Name                   | R/W | Default      | Description                     |
|--------|------------------------|-----|--------------|---------------------------------|
| 0x200  | STATS_FRAMES           | RO  | 0x00000000   | Total frames completed          |
| 0x204  | STATS_BYTES            | RO  | 0x00000000   | Total bytes transferred         |
| 0x208  | STATS_ERRORS           | RO  | 0x00000000   | Total error count               |
| 0x20C  | STATS_DROPPED          | RO  | 0x00000000   | Total dropped frames            |

### 6.7 CTRL Register (0x000) Bit Definitions

| Bit | Name        | Description                              |
|-----|-------------|------------------------------------------|
| 0   | ENABLE      | Enable the DMA engine                    |
| 1   | STREAM      | Start DMA transfers                      |
| 2   | RING_ENABLE | Enable descriptor ring operation         |

### 6.8 STATUS Register (0x004) Bit Definitions

| Bit | Name      | Description                              |
|-----|-----------|------------------------------------------|
| 0   | READY     | DMA engine is powered and ready          |
| 1   | STREAMING | DMA engine is actively transferring      |

### 6.9 Interrupt Bits (INT_MASK / INT_STATUS)

| Bit | Name        | Description                              |
|-----|-------------|------------------------------------------|
| 0   | FRAME_DONE  | Frame DMA transfer complete              |
| 1   | ERROR       | DMA transfer error                       |
| 2   | OVERFLOW    | Ring buffer overflow                     |

### 6.10 Ring Buffer Operation

The DMA engine uses a circular descriptor ring for buffer management:

```
  Driver (HEAD)                      Hardware (TAIL)
       |                                  |
       v                                  v
  +--------+--------+--------+--------+--------+---
  | desc 0 | desc 1 | desc 2 | desc 3 | desc 4 | ...
  +--------+--------+--------+--------+--------+---

  Descriptors between TAIL and HEAD are owned by hardware.
  Driver advances HEAD by setting OWN flag on new descriptors.
  Hardware advances TAIL after completing each frame.
  Ring wraps at BUF_RING_SIZE.
```

**Protocol:**
1. Driver allocates descriptor ring and calls `vsoc_hw_set_buf_ring()`
2. Driver fills descriptors with buffer addresses and sets `OWN` flag
3. Driver advances `BUF_RING_HEAD` to submit new buffers
4. Hardware processes descriptors from `TAIL` to `HEAD`
5. Hardware clears `OWN`, sets `DONE`, writes sequence number
6. Hardware advances `BUF_RING_TAIL`
7. Hardware fires `FRAME_DONE` interrupt (if unmasked)

---

## 7. Buffer Descriptor Format

### 7.1 Descriptor Layout (16 bytes)

```
  struct vsoc_hw_desc {
      u32 addr_lo;    /* +0x00: Buffer physical address, low 32 bits  */
      u32 addr_hi;    /* +0x04: Buffer physical address, high 32 bits */
      u32 size;       /* +0x08: Buffer size in bytes                  */
      u32 flags;      /* +0x0C: Ownership and status flags            */
  };
```

```
  Byte Offset   Field       Size    Description
  ──────────────────────────────────────────────────
  0x00          addr_lo     4       Buffer address bits [31:0]
  0x04          addr_hi     4       Buffer address bits [63:32]
  0x08          size        4       Buffer capacity in bytes
  0x0C          flags       4       Status and sequence (see below)
```

### 7.2 Flags Field Bit Definitions

```
  Bit 31      OWN       1 = descriptor owned by hardware
  Bit 30      DONE      1 = frame written to buffer
  Bit 29      ERROR     1 = error on this frame
  Bits 28:16  Reserved
  Bits 15:0   SEQ_MASK  Frame sequence number (0-65535)
```

| Bit(s) | Name     | Mask         | Description                       |
|--------|----------|--------------|-----------------------------------|
| 31     | OWN      | `0x80000000` | Hardware ownership flag           |
| 30     | DONE     | `0x40000000` | Frame complete flag               |
| 29     | ERROR    | `0x20000000` | Error flag                        |
| 15:0   | SEQ_MASK | `0x0000FFFF` | Frame sequence number             |

### 7.3 Ownership Handshake Protocol

```
  Driver prepares buffer:
    1. Write buffer virtual address to addr_lo / addr_hi
    2. Write buffer capacity to size
    3. Set flags = OWN (0x80000000)
    4. Advance HEAD pointer

  Hardware completes frame:
    1. Write test pattern into buffer
    2. Clear OWN bit
    3. Set DONE bit
    4. Write sequence number into bits [15:0]
    5. Advance TAIL pointer
    6. Fire FRAME_DONE interrupt

  Driver receives interrupt:
    1. Read descriptor at TAIL
    2. Check DONE bit -> frame is ready
    3. Check ERROR bit -> handle error
    4. Read SEQ_MASK -> frame sequence number
    5. Process buffer
    6. Recycle descriptor (set OWN, re-queue)
```

---

## 8. Interrupt Architecture

### 8.1 DMA IRQ

The VSOC-3000 uses a single software-allocated IRQ for the DMA engine.
The IRQ number is dynamically assigned at module load time via
`irq_alloc_desc()` and retrieved by drivers via `vsoc_hw_get_dma_irq()`.

The IRQ uses `dummy_irq_chip` with `handle_simple_irq` handler.

### 8.2 Interrupt Mask Convention

All interrupt mask registers follow the same convention:

- **Mask bit = 1**: Interrupt source is **disabled** (masked)
- **Mask bit = 0**: Interrupt source is **enabled** (unmasked)

Default mask value is `0xFFFFFFFF` --- all interrupts disabled at reset.

To enable an interrupt, clear the corresponding mask bit:

```c
  /* Enable FRAME_DONE interrupt */
  u32 mask = ioread32(dma_regs + VSOC_DMA_INT_MASK);
  mask &= ~VSOC_DMA_INT_FRAME_DONE;
  iowrite32(mask, dma_regs + VSOC_DMA_INT_MASK);
```

### 8.3 Write-1-to-Clear (W1C) Status

Interrupt status registers use W1C semantics:

- Reading returns currently pending interrupt bits
- Writing a `1` to a bit clears that pending interrupt
- Writing a `0` to a bit has no effect

```c
  /* Acknowledge FRAME_DONE interrupt */
  u32 status = ioread32(dma_regs + VSOC_DMA_INT_STATUS);
  if (status & VSOC_DMA_INT_FRAME_DONE) {
      iowrite32(VSOC_DMA_INT_FRAME_DONE,
                dma_regs + VSOC_DMA_INT_STATUS);
      /* Handle frame done ... */
  }
```

### 8.4 Interrupt Delivery Logic

```
  INT_STATUS bits are set by hardware events.
  Effective interrupt = INT_STATUS & ~INT_MASK
  If effective interrupt is non-zero, fire IRQ via generic_handle_irq_safe().
  After delivery, the status bits that triggered the IRQ are auto-cleared.
```

---

## 9. Test Pattern Generator

### 9.1 Color Bar Pattern

The VSOC-3000 generates an 8-bar color test pattern identical to the
SMPTE color bar standard layout:

```
  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
  │White │Yellow│ Cyan │Green │Magenta│ Red  │ Blue │Black │
  │(255, │(255, │(0,   │(255, │(255, │(0,   │(0,   │(0,   │
  │ 255, │ 255, │ 255, │ 255, │ 0,   │ 0,   │ 0,   │ 0,   │
  │ 255) │ 0)   │ 255) │ 0)   │ 255) │ 0)   │ 255) │ 0)   │
  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
```

**Note:** Bar 0 is white (R=255,G=255,B=255) and bar 7 is black
(R=0,G=0,B=0).  Each bar width is `image_width / 8` pixels.

### 9.2 Color Bar RGB Values

| Bar | Color   | R   | G   | B   |
|-----|---------|-----|-----|-----|
| 0   | White   | 255 | 255 | 255 |
| 1   | Yellow  | 255 | 255 | 0   |
| 2   | Cyan    | 0   | 255 | 255 |
| 3   | Green   | 255 | 255 | 0   |
| 4   | Magenta | 255 | 0   | 255 |
| 5   | Red     | 0   | 0   | 0   |
| 6   | Blue    | 0   | 0   | 255 |
| 7   | Black   | 0   | 0   | 0   |

### 9.3 Animation

The red channel is animated per-frame using the sequence number:

```
  R_final = (R_bar + brightness_offset + (sequence & 0x1F)) & 0xFF
```

This produces a subtle red-channel shimmer that cycles every 32 frames,
useful for visually confirming that frames are being generated.

### 9.4 Brightness Effect

ISP brightness (register `VSOC_ISP_BRIGHTNESS`) is applied as an offset
to all three RGB channels:

```
  offset = brightness_register_value - 128
  R = R_bar + offset    (clamped to [0, 255])
  G = G_bar + offset    (clamped to [0, 255])
  B = B_bar + offset    (clamped to [0, 255])
```

Default brightness is 128 (no offset).

### 9.5 Horizontal Flip Effect

When the sensor `HFLIP` register (0x12) is set to 1, each scanline is
rendered right-to-left, mirroring the color bar pattern horizontally.

### 9.6 Frame Counter Embedding

The first 4 bytes of every frame buffer are overwritten with the
32-bit frame counter value (little-endian).  This allows userspace
tools to verify frame ordering and detect dropped frames.

```
  buf[0..3] = (u32) frame_sequence_number
```

---

## 10. Platform Module Interface

### 10.1 Exported Functions

The `soc_hw_platform.ko` module exports the following symbols
(`EXPORT_SYMBOL_GPL`) for use by driver modules:

#### I2C Adapter

| Function                        | Returns             | Description                              |
|---------------------------------|---------------------|------------------------------------------|
| `vsoc_hw_get_i2c_adapter()`    | `struct i2c_adapter *` | Get the virtual I2C adapter for sensor access. Returns NULL if platform not loaded. |

#### MMIO Register Maps

| Function                        | Returns             | Description                              |
|---------------------------------|---------------------|------------------------------------------|
| `vsoc_hw_map_csi2_regs()`      | `void __iomem *`   | Map CSI-2 register block                 |
| `vsoc_hw_map_isp_regs()`       | `void __iomem *`   | Map ISP register block                   |
| `vsoc_hw_map_dma_regs()`       | `void __iomem *`   | Map DMA engine register block            |
| `vsoc_hw_unmap_csi2_regs()`    | `void`              | Unmap CSI-2 register block               |
| `vsoc_hw_unmap_isp_regs()`     | `void`              | Unmap ISP register block                 |
| `vsoc_hw_unmap_dma_regs()`     | `void`              | Unmap DMA engine register block          |

**Note:** The "map" functions return pointers to in-memory register
arrays.  The "unmap" functions are no-ops in the virtual implementation
but must be called for API correctness.

#### DMA IRQ

| Function                        | Returns             | Description                              |
|---------------------------------|---------------------|------------------------------------------|
| `vsoc_hw_get_dma_irq()`        | `int`               | Get the DMA software IRQ number. Returns `-ENODEV` if platform not loaded. |

#### DMA Buffer Ring

| Function                           | Returns | Description                              |
|------------------------------------|---------|------------------------------------------|
| `vsoc_hw_set_buf_ring(va, count)`  | `int`   | Register descriptor ring. `va` = virtual address of descriptor array, `count` = number of descriptors (1 to 16). Returns 0 on success, `-EINVAL` if invalid, `-ENODEV` if not loaded. |
| `vsoc_hw_clear_buf_ring()`         | `void`  | Deregister descriptor ring and reset pointers. |

#### Streaming

| Function                           | Returns | Description                              |
|------------------------------------|---------|------------------------------------------|
| `vsoc_hw_notify_stream(int on)`    | `void`  | Start (`on=1`) or stop (`on=0`) the frame generation engine. Starts a 33ms periodic workqueue on start; cancels synchronously on stop. |

#### Platform Devices

| Function                           | Returns                  | Description                    |
|------------------------------------|--------------------------|--------------------------------|
| `vsoc_hw_get_bridge_pdev()`        | `struct platform_device *` | Get bridge platform device   |
| `vsoc_hw_get_csi2_pdev()`          | `struct platform_device *` | Get CSI-2 platform device    |
| `vsoc_hw_get_isp_pdev()`           | `struct platform_device *` | Get ISP platform device      |

### 10.2 Load Order

```
  # Step 1: Load the hardware platform (must be first)
  sudo insmod soc/hw/soc_hw_platform.ko

  # Step 2: Load driver modules (any order after platform)
  sudo insmod soc/partN/vsoc_*.ko
```

### 10.3 Module Dependency Chain

```
  soc_hw_platform.ko  (base — no dependencies)
       |
       +--- Provides: I2C adapter, MMIO maps, DMA IRQ, buffer ring API
       |
       +--- Required by: all vsoc_*.ko driver modules
```

All driver modules depend on `soc_hw_platform.ko`.  If the platform
module is not loaded, all exported functions return NULL or `-ENODEV`.

---

## 11. Programming Sequences

### 11.1 Sensor Initialization

```
  1. adapter = vsoc_hw_get_i2c_adapter()
  2. i2c_smbus_read_word_data(client, CHIP_ID)   -> expect 0x3000
  3. i2c_smbus_read_word_data(client, CHIP_REV)   -> expect 0x01
  4. i2c_smbus_read_word_data(client, STATUS)      -> verify READY
  5. i2c_smbus_write_word_data(client, CTRL, ENABLE)
```

### 11.2 Pipeline Startup Sequence

The pipeline must be started in forward order (upstream to downstream):

```
  Step 1 — Sensor:
    Write WIDTH, HEIGHT, FMT
    Write CTRL = ENABLE | STREAM

  Step 2 — CSI-2 Receiver:
    csi2_regs = vsoc_hw_map_csi2_regs()
    Write FMT, WIDTH, HEIGHT, LANES
    Write CTRL = ENABLE | STREAM
    Unmask desired interrupts

  Step 3 — ISP:
    isp_regs = vsoc_hw_map_isp_regs()
    Write IN_FMT, OUT_FMT, WIDTH, HEIGHT
    Write BRIGHTNESS, CONTRAST (or use defaults)
    Write CTRL = ENABLE | STREAM
    Unmask desired interrupts

  Step 4 — DMA Engine:
    dma_regs = vsoc_hw_map_dma_regs()
    Write FMT_WIDTH, FMT_HEIGHT, FMT_STRIDE, FMT_FRAMESIZE
    Allocate descriptor ring
    vsoc_hw_set_buf_ring(ring_va, count)
    Fill descriptors with OWN flag, advance HEAD
    Unmask DMA interrupts (clear mask bits)
    Write CTRL = ENABLE | STREAM | RING_ENABLE
    vsoc_hw_notify_stream(1)    <- starts frame generation
```

### 11.3 Pipeline Shutdown Sequence

Shutdown proceeds in reverse order (downstream to upstream):

```
  Step 1 — DMA Engine:
    vsoc_hw_notify_stream(0)    <- stops frame generation
    Write CTRL = 0
    Mask all DMA interrupts
    vsoc_hw_clear_buf_ring()
    vsoc_hw_unmap_dma_regs()

  Step 2 — ISP:
    Write CTRL = 0
    Mask all ISP interrupts
    vsoc_hw_unmap_isp_regs()

  Step 3 — CSI-2 Receiver:
    Write CTRL = 0
    Mask all CSI-2 interrupts
    vsoc_hw_unmap_csi2_regs()

  Step 4 — Sensor:
    Write CTRL = 0 (clear STREAM and ENABLE)
```

### 11.4 Format Configuration Sequence

```
  1. Stop streaming (if active)
  2. Write sensor: WIDTH, HEIGHT, FMT
  3. Write CSI-2:  WIDTH, HEIGHT, FMT
  4. Write ISP:    WIDTH, HEIGHT, IN_FMT, OUT_FMT
  5. Write DMA:    FMT_WIDTH, FMT_HEIGHT, FMT_STRIDE, FMT_FRAMESIZE
     where:
       FMT_STRIDE    = width * bytes_per_pixel
       FMT_FRAMESIZE = FMT_STRIDE * height
  6. Restart streaming
```

---

## 12. Electrical Specifications (Virtual)

The VSOC-3000 has no real electrical characteristics.  The following
virtual timing and capacity parameters apply:

### 12.1 Timing

| Parameter              | Value            | Notes                        |
|------------------------|------------------|------------------------------|
| Frame interval         | 33 ms            | ~30.3 fps                    |
| Frame generation       | delayed_work     | Kernel workqueue timer       |
| Timestamp source       | `ktime_get()`    | Monotonic nanoseconds        |
| Interrupt latency      | ~0               | Software IRQ, no bus delay   |

### 12.2 Capacity

| Parameter              | Value            | Notes                        |
|------------------------|------------------|------------------------------|
| Max ring size          | 16 descriptors   | `VSOC_HW_RING_MAX`          |
| Max resolution         | 3840 x 2160      | 4K UHD                       |
| Min resolution         | 160 x 120        | QQVGA                        |
| Resolution step        | 16 pixels        | Width and height             |
| Max frame size         | ~24 MB           | 3840 * 3 * 2160 (RGB24)     |
| Descriptor size        | 16 bytes         | `struct vsoc_hw_desc`        |

### 12.3 Register Space Sizes

| Block       | Registers | Width   | Total Size |
|-------------|-----------|---------|------------|
| Sensor      | 16        | 16-bit  | 32 bytes   |
| CSI-2       | 16        | 32-bit  | 64 bytes   |
| ISP         | 16        | 32-bit  | 64 bytes   |
| DMA Engine  | 144       | 32-bit  | 576 bytes  |

### 12.4 Platform Resources Created

| Resource                | Type                | Name / ID              |
|-------------------------|---------------------|------------------------|
| I2C Adapter             | Virtual I2C bus     | `vsoc-i2c` (auto-nr)  |
| I2C Client              | Virtual sensor      | `vsoc_sensor @ 0x10`  |
| Platform Device         | Bridge              | `vsoc_bridge`          |
| Platform Device         | CSI-2               | `vsoc_csi2`            |
| Platform Device         | ISP                 | `vsoc_isp`             |
| IRQ Descriptor          | Software IRQ        | Dynamic allocation     |
| Delayed Work            | Frame generator     | 33ms periodic          |

---

*Copyright (c) 2024 TECH VEDA. All rights reserved.*
*Author: Raghu Bharadwaj*
*VSOC-3000 Virtual SoC Camera Platform Datasheet, Revision 1.0*
