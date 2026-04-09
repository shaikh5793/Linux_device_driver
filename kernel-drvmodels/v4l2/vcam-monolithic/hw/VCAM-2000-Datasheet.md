# VCAM-2000 Virtual Image Sensor Controller — Datasheet

## 1. Overview

The VCAM-2000 is a virtual image sensor controller designed for the V4L2
driver curriculum.  It models the register interface, interrupt behavior,
and DMA patterns found in real camera sensor/ISP hardware.

```
  ┌──────────────────────────────────────────────────┐
  │                   VCAM-2000                       │
  │                                                   │
  │  ┌──────────┐  ┌───────────┐  ┌──────────────┐  │
  │  │  Sensor   │  │  Image    │  │  DMA Engine  │  │
  │  │  Core     │──│  Pipeline │──│  (Ring-based) │──── Frame Data
  │  │  (30fps)  │  │  (bright, │  │              │  │   to Memory
  │  │           │  │   hflip)  │  │              │  │
  │  └──────────┘  └───────────┘  └──────┬───────┘  │
  │                                       │          │
  │  ┌──────────────────────────────────┐ │          │
  │  │   Register File (128 × 32-bit)  │ │          │
  │  │   ioread32 / iowrite32 access   │ │          │
  │  └──────────────────────────────────┘ │          │
  │                                       │          │
  │  ┌──────────────────────────────────┐ │          │
  │  │   Interrupt Controller           │ │          │
  │  │   FRAME_DONE / ERROR / OVERFLOW  │─┘          │
  │  └──────────────────────────────────┘            │
  └──────────────────────────────────────────────────┘
```

**Key features:**
  - 32-bit register file accessed via ioread32/iowrite32
  - Descriptor ring for frame buffer delivery (head/tail pointers)
  - Software interrupt with mask/status registers
  - Configurable image pipeline (brightness, hflip)
  - ~30 fps frame generation engine
  - RGB24 output format (640×480 default)

**Chip identification:**
  - CHIP_ID: 0x00CA2000
  - CHIP_REV: 0x01

---

## 2. Register Map

All registers are 32-bit, aligned to 4-byte boundaries.

### 2.1 Core Control Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x000  | CTRL           | R/W    | 0x00000000   | Sensor control               |
| 0x004  | STATUS         | R      | 0x00000001   | Sensor status                |
| 0x008  | INT_MASK       | R/W    | 0xFFFFFFFF   | Interrupt mask (1=disabled)  |
| 0x00C  | INT_STATUS     | R/W1C  | 0x00000000   | Interrupt status (W1C)       |
| 0x010  | CHIP_ID        | R      | 0x00CA2000   | Chip identification          |
| 0x014  | CHIP_REV       | R      | 0x00000001   | Chip revision                |

### 2.2 Format Configuration Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x020  | FMT_WIDTH      | R/W    | 640          | Frame width in pixels        |
| 0x024  | FMT_HEIGHT     | R/W    | 480          | Frame height in lines        |
| 0x028  | FMT_PIXFMT     | R/W    | 0x33424752   | Pixel format (V4L2 fourcc)   |
| 0x02C  | FMT_STRIDE     | R/W    | 1920         | Bytes per line               |
| 0x030  | FMT_FRAMESIZE  | R/W    | 921600       | Total frame size in bytes    |

### 2.3 Image Control Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x040  | BRIGHTNESS     | R/W    | 128          | Brightness offset (0–255)    |
| 0x044  | HFLIP          | R/W    | 0            | Horizontal flip (0 or 1)     |

### 2.4 Buffer Descriptor Ring Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x100  | BUF_RING_ADDR  | R/W    | 0            | Ring base address            |
| 0x104  | BUF_RING_SIZE  | R/W    | 0            | Number of ring entries       |
| 0x108  | BUF_RING_HEAD  | R/W    | 0            | Driver write pointer         |
| 0x10C  | BUF_RING_TAIL  | R      | 0            | Hardware read pointer        |

### 2.5 Frame Status Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x0C0  | FRAME_COUNT    | R      | 0            | Frames captured since stream |
| 0x0C4  | FRAME_TS_LO    | R      | 0            | Last frame timestamp (low)   |
| 0x0C8  | FRAME_TS_HI    | R      | 0            | Last frame timestamp (high)  |

### 2.6 Statistics Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x200  | STATS_FRAMES   | R      | 0            | Total frames captured        |
| 0x204  | STATS_BYTES    | R      | 0            | Total bytes transferred      |
| 0x208  | STATS_ERRORS   | R      | 0            | Error count                  |
| 0x20C  | STATS_DROPPED  | R      | 0            | Dropped frame count          |

---

## 3. Register Bit Fields

### 3.1 CTRL Register (0x000)

```
  Bit    Name           Description
  ─────────────────────────────────────────
   0     ENABLE         Power on sensor
   1     STREAM_ON      Start frame capture
   4     RESET          Software reset (self-clearing)
   5     RING_ENABLE    Enable descriptor ring mode
```

**Typical sequences:**
  - Power on:  write `ENABLE`
  - Start capture:  write `ENABLE | STREAM_ON | RING_ENABLE`
  - Stop capture:  write `0`
  - Reset:  write `RESET` (clears all state)

### 3.2 STATUS Register (0x004)

```
  Bit    Name           Description
  ─────────────────────────────────────────
   0     READY          Sensor initialized
   1     STREAMING      Currently capturing frames
```

### 3.3 Interrupt Mask / Status (0x008, 0x00C)

```
  Bit    Name           Description
  ─────────────────────────────────────────
   0     FRAME_DONE     Frame captured and DMA complete
   1     ERROR          DMA fault or pipeline error
   2     OVERFLOW       Ring buffer full, frame dropped
```

**Mask convention:** 1 = interrupt source disabled, 0 = enabled.

**Acknowledgment:** Write 1 to INT_STATUS bits to clear them (W1C).

**Interrupt delivery pipeline:**

```
  Event (frame done)
    → Set INT_STATUS bit
    → Check INT_MASK
    → If (INT_STATUS & ~INT_MASK) != 0: fire IRQ
    → ISR reads INT_STATUS
    → ISR writes INT_STATUS to acknowledge
    → ISR schedules bottom-half work
```

---

## 4. Buffer Descriptor Format

Each descriptor is 16 bytes (4 × 32-bit words):

```
  Word 0: addr_lo    Buffer address (low 32 bits)
  Word 1: addr_hi    Buffer address (high 32 bits)
  Word 2: size       Buffer size in bytes
  Word 3: flags      Ownership and status flags
```

### 4.1 Descriptor Flag Bits

```
  Bit     Name       Description
  ──────────────────────────────────────────
   31     OWN        1 = hardware owns this descriptor
   30     DONE       Frame has been written to buffer
   29     ERROR      Error during frame capture
   15:0   SEQ        Sequence number (set by hardware)
```

### 4.2 Ownership Handshake

```
  Driver                          Hardware
  ──────                          ────────
  Fill desc (addr, size)
  Set OWN flag
  Advance HEAD register
                                  Detect HEAD != TAIL
                                  Read desc at TAIL
                                  Check OWN flag set
                                  Generate frame → buffer
                                  Clear OWN, set DONE + seq
                                  Advance TAIL register
                                  Fire FRAME_DONE interrupt
  ISR: read INT_STATUS
  Work: read TAIL register
  Walk from saved tail to TAIL
  For each DONE desc:
    Complete VB2 buffer
  Update saved tail
```

---

## 5. Frame Capture Operation

### 5.1 Ring Setup (driver start_streaming)

1. Allocate descriptor ring in kernel memory (kcalloc)
2. Allocate per-slot buffer tracking array
3. Call `vcam_hw_set_buf_ring(ring_va, count)` to register with hardware
4. Program format registers (FMT_WIDTH, FMT_HEIGHT, etc.)
5. Unmask FRAME_DONE interrupt in INT_MASK
6. Write `ENABLE | STREAM_ON | RING_ENABLE` to CTRL
7. Call `vcam_hw_notify_ctrl()` for immediate effect

### 5.2 Buffer Submission (driver buf_queue)

1. Get buffer virtual address from VB2
2. Fill descriptor: addr_lo/hi, size, flags = OWN
3. Store VB2 buffer pointer in tracking array
4. Advance HEAD pointer
5. Write HEAD to BUF_RING_HEAD register (doorbell)

### 5.3 Frame Generation (hardware internal)

1. Timer fires at ~30 fps
2. Check CTRL: ENABLE, STREAM_ON, RING_ENABLE all set
3. Compare HEAD and TAIL — if equal, ring is empty
4. Read descriptor at TAIL position
5. Verify OWN flag is set
6. Read format registers for width/height/stride
7. Read BRIGHTNESS/HFLIP for image processing
8. Generate 8-bar color pattern into buffer
9. Embed frame counter in first 4 bytes
10. Clear OWN, set DONE flag with sequence number
11. Update FRAME_COUNT, FRAME_TS_LO/HI, statistics
12. Advance TAIL pointer
13. Fire FRAME_DONE interrupt if unmasked

### 5.4 Frame Completion (driver ISR + work)

1. ISR reads INT_STATUS — if FRAME_DONE bit set:
   - Write INT_STATUS back to acknowledge (W1C)
   - Schedule work
2. Work function:
   - Read BUF_RING_TAIL from hardware
   - Walk from driver's saved tail to hardware tail
   - For each completed descriptor:
     - Set VB2 buffer timestamp and sequence
     - Call vb2_buffer_done(DONE or ERROR)
   - Update driver's saved tail

### 5.5 Ring State Diagram

```
  HEAD=0, TAIL=0: Ring empty (no buffers submitted)

  After driver submits 3 buffers:
  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
  │ OWN │ OWN │ OWN │     │     │     │     │     │
  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
    T=0                H=3

  After hardware processes 2 frames:
  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
  │DONE │DONE │ OWN │     │     │     │     │     │
  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
                T=2   H=3
```

---

## 6. Image Controls

### 6.1 Brightness (0x040)

Range: 0–255, default 128.  Applied as offset to each color channel.
The hardware subtracts 128 from the register value to get a signed
offset (-128 to +127), then adds it to each RGB value with clamping.

### 6.2 Horizontal Flip (0x044)

Values: 0 = normal, 1 = mirror.  When enabled, the hardware reads
pixels right-to-left when generating each scanline.

---

## 7. Test Pattern

The VCAM-2000 generates an 8-bar color pattern:

```
  Bar  Color    R    G    B
  ──────────────────────────
   0   White   255  255  255
   1   Yellow  255  255    0
   2   Cyan      0  255  255
   3   Green   255  255    0
   4   Magenta 255    0  255
   5   Red       0    0    0
   6   Blue      0    0  255
   7   Black     0    0    0
```

The red channel is animated with the frame counter for visual feedback.
The frame counter (sequence number) is embedded in the first 4 bytes
of each frame buffer.

---

## 8. Platform Module Interface

The hardware platform is provided by `vcam_hw_platform.ko`.  Driver
modules use these exported functions:

| Function                | First Used | Purpose                         |
|-------------------------|------------|---------------------------------|
| `vcam_hw_map_regs()`    | Part 2     | Get __iomem pointer to reg file |
| `vcam_hw_unmap_regs()`  | Part 2     | Release register mapping        |
| `vcam_hw_get_irq()`     | Part 3     | Get software IRQ number         |
| `vcam_hw_set_buf_ring()`| Part 3     | Register descriptor ring        |
| `vcam_hw_clear_buf_ring()`| Part 3   | Tear down descriptor ring       |
| `vcam_hw_notify_ctrl()` | Part 3     | Notify hw of CTRL register write|
| `vcam_hw_get_pdev()`    | Part 3     | Get platform_device for logging |

---

## 9. Driver Lifecycle

### 9.1 Module Load

```
  insmod vcam_hw_platform.ko
    → Allocates register file
    → Allocates software IRQ
    → Creates platform device "vcam_hw"
    → Initializes registers to power-on defaults

  insmod vcam_<part>.ko
    → Registers platform driver
    → probe() called:
        vcam_hw_map_regs()    → get register pointer
        ioread32(CHIP_ID)     → verify hardware
        vcam_hw_get_irq()     → get IRQ number
        request_irq()         → install handler
        v4l2_device_register()
        vb2_queue_init()
        video_register_device()
```

### 9.2 Streaming Start

```
  VIDIOC_STREAMON
    → start_streaming():
        kcalloc(ring)
        vcam_hw_set_buf_ring(ring, count)
        iowrite32(format regs)
        iowrite32(~FRAME_DONE, INT_MASK)  → unmask interrupt
        iowrite32(ENABLE|STREAM_ON|RING_ENABLE, CTRL)
        vcam_hw_notify_ctrl()             → start hw timer
```

### 9.3 Streaming Stop

```
  VIDIOC_STREAMOFF
    → stop_streaming():
        iowrite32(0, CTRL)
        vcam_hw_notify_ctrl()             → stop hw timer
        iowrite32(0xFFFFFFFF, INT_MASK)   → mask all interrupts
        cancel_work_sync()
        return pending buffers to VB2
        vcam_hw_clear_buf_ring()
        kfree(ring)
```

---

## 10. Hardware Constants

| Constant          | Value      | Description                |
|-------------------|------------|----------------------------|
| CHIP_ID           | 0x00CA2000 | Chip identification        |
| CHIP_REV          | 0x01       | Revision number            |
| REG_COUNT         | 128        | Number of 32-bit registers |
| FRAME_MS          | 33         | Frame period (~30 fps)     |
| MIN_WIDTH         | 160        | Minimum frame width        |
| MAX_WIDTH         | 1920       | Maximum frame width        |
| MIN_HEIGHT        | 120        | Minimum frame height       |
| MAX_HEIGHT        | 1080       | Maximum frame height       |
| DEF_WIDTH         | 640        | Default frame width        |
| DEF_HEIGHT        | 480        | Default frame height       |
| STEP              | 16         | Width/height alignment     |
| RING_MAX          | 16         | Maximum ring entries       |
| FMT_RGB24         | 0x33424752 | RGB24 pixel format code    |
