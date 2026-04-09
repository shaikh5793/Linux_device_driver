# VCAM-2000 Virtual Image Sensor Controller -- Streaming Datasheet
## Part 3: Interrupts, Descriptor Ring & Frame Capture

Copyright (c) 2024 TECH VEDA / Author: Raghu Bharadwaj

**Applicable to**: Part 3 (VB2 Streaming with Interrupt-Driven Frame Capture)

This document describes the interrupt controller, buffer descriptor ring, and
frame capture subsystems of the VCAM-2000 as used by the Part 3 driver.
It includes all Part 2 register content plus the streaming data path.

---

## 1. System Architecture

Part 3 introduces the DMA data path (frame capture via descriptor ring) and
hardware interrupts.  The sensor core generates frames at ~30 fps and writes
them into buffers described by a ring of descriptors.

```
  +-------------------------+          +-----------------------------------+
  |       CPU               |          |          Kernel Memory             |
  |                         |          |                                   |
  |  ioread32/iowrite32 <---+----------+--- Register File (MMIO)          |
  |                         |          |                                   |
  |  vcam_isr() <----- IRQ-+          |  +-----------------------------+ |
  |     |                   |          |  | Buffer Descriptor Ring      | |
  |     +-> work function   |          |  | (kcalloc)                   | |
  |         walk ring,      |          |  | N x 16 bytes               | |
  |         complete bufs   |          |  +-----------------------------+ |
  |                         |          |  | VB2 Frame Buffers           | |
  |  buf_queue() ----------+--submit-->|  | (vb2_plane_vaddr)          | |
  |    fill descriptor,    |          |  | N x FMT_FRAMESIZE bytes    | |
  |    advance HEAD        |          |  +-----------------------------+ |
  +-------------------------+          +-----------------------------------+
            |
  +---------v------------------------------------------------+
  |                 VCAM-2000 Controller                      |
  |                                                          |
  |  +-----------+  +-----------+  +----------------------+  |
  |  | Sensor    |  | Image     |  | DMA Engine           |  |
  |  | Core      |  | Pipeline  |  | (Ring-based)         |  |
  |  | (~30fps)  |  | (bright,  |  |                      |  |
  |  |           |  |  hflip)   |  | Reads descriptors    |  |
  |  +-----------+  +-----------+  | Writes frame data    |  |
  |                                | into buffers         |  |
  |  +-------------+              +----------+-----------+  |
  |  | Register    |              | Interrupt Controller |  |
  |  | File        |              | FRAME_DONE / ERROR   |  |
  |  | CTRL  0x000 |              | / OVERFLOW           |  |
  |  | STATUS 0x004|              +----------------------+  |
  |  +-------------+                                        |
  +----------------------------------------------------------+
```

---

## 2. Complete Register Map (Part 3)

All registers are 32-bit, little-endian, accessed through MMIO.

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

### 2.3 Buffer Descriptor Ring Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x100  | BUF_RING_ADDR  | R/W    | 0            | Ring base address            |
| 0x104  | BUF_RING_SIZE  | R/W    | 0            | Number of ring entries       |
| 0x108  | BUF_RING_HEAD  | R/W    | 0            | Driver write pointer         |
| 0x10C  | BUF_RING_TAIL  | R      | 0            | Hardware read pointer        |

### 2.4 Frame Status Registers

| Offset | Name           | Access | Reset Value  | Description                  |
|--------|----------------|--------|--------------|------------------------------|
| 0x0C0  | FRAME_COUNT    | R      | 0            | Frames captured since stream |
| 0x0C4  | FRAME_TS_LO    | R      | 0            | Last frame timestamp (low)   |
| 0x0C8  | FRAME_TS_HI    | R      | 0            | Last frame timestamp (high)  |

---

## 3. CTRL Register (0x000) Bit Fields

```
 31                              16 15        5  4     1  0
+----------------------------------+----------+--+-----+--+
|            Reserved (0)          | Rsvd (0) |RE|Rsvd |SO|EN|
+----------------------------------+----------+--+-----+--+--+
                                                |       |  |
                                                |       |  +-- Bit 0: ENABLE
                                                |       +----- Bit 1: STREAM_ON
                                                +------------- Bit 5: RING_ENABLE
```

### 3.1 Bit Definitions

```
  Bit    Name           Description
  ---------------------------------------------------------
   0     ENABLE         Power on sensor
   1     STREAM_ON      Start frame capture
   4     RESET          Software reset (self-clearing)
   5     RING_ENABLE    Enable descriptor ring mode
```

### 3.2 Part 3 Usage

| Bit | Name        | Part 3 role                                          |
|-----|-------------|------------------------------------------------------|
|  0  | ENABLE      | Set in probe/open, cleared in stop/remove             |
|  1  | STREAM_ON   | Set in start_streaming, cleared in stop_streaming     |
|  4  | RESET       | Write to reset all hardware state                     |
|  5  | RING_ENABLE | Set in start_streaming to enable ring processing      |

### 3.3 Typical Sequences

```
  Power on:       iowrite32(ENABLE, CTRL)                    // 0x01
  Start capture:  iowrite32(ENABLE | STREAM_ON | RING_ENABLE, CTRL)  // 0x23
  Stop capture:   iowrite32(0, CTRL)
  Reset:          iowrite32(RESET, CTRL)                     // 0x10
```

---

## 4. STATUS Register (0x004) Bit Fields

```
  Bit    Name           Description
  ---------------------------------------------------------
   0     READY          Sensor initialized
   1     STREAMING      Currently capturing frames
```

STREAMING (bit 1) is set by hardware when CTRL has ENABLE, STREAM_ON,
and RING_ENABLE all set and at least one descriptor is available.

---

## 5. Interrupt Controller

### 5.1 INT_MASK Register (0x008)

Controls which interrupt sources are enabled.

```
 31                                        3  2  1  0
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|            Reserved (0)                  |OV|ER|FD|
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
                                            |  |  |
                                            |  |  +-- FRAME_DONE mask (bit 0)
                                            |  +----- ERROR mask (bit 1)
                                            +-------- OVERFLOW mask (bit 2)

  Convention:  1 = DISABLED (masked)    0 = ENABLED (unmasked)
```

| Bit | Name            | Description                          |
|-----|-----------------|--------------------------------------|
| 0   | FRAME_DONE      | 1 = masked (disabled), 0 = enabled   |
| 1   | ERROR           | 1 = masked (disabled), 0 = enabled   |
| 2   | OVERFLOW        | 1 = masked (disabled), 0 = enabled   |
| 3-31| Reserved        | Must be written as 1 (disabled)      |

Write `0xFFFFFFFF` to disable all interrupts (power-on default).
Write `~FRAME_DONE` (0xFFFFFFFE) to enable only FRAME_DONE.

### 5.2 INT_STATUS Register (0x00C)

Reports pending interrupt events.  **Write-1-to-clear** semantics: writing
a 1 to a bit clears that pending event.

```
 31                                        3  2  1  0
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|            Reserved (0)                  |OV|ER|FD|
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
                                            |  |  |
                                            |  |  +-- FRAME_DONE (bit 0)
                                            |  +----- ERROR (bit 1)
                                            +-------- OVERFLOW (bit 2)
```

### 5.3 Interrupt Delivery Condition

An interrupt is delivered to the CPU when:

```
  (INT_STATUS & ~INT_MASK) != 0
```

At least one unmasked status bit must be set.

### 5.4 Interrupt Delivery Pipeline

```
  Hardware Event (frame done, error, overflow)
       |
       v
  +------------------+
  | Set INT_STATUS   |     INT_STATUS register
  | bit for event    |     (latches event)
  +--------+---------+
           |
           v
  +------------------+
  | Check INT_MASK   |     Is the corresponding
  | bit clear?       |     mask bit 0 (enabled)?
  +--------+---------+
           |
      Yes  |  No
           |  +---> Event recorded but no IRQ
           v
  +------------------+
  | Fire IRQ         |     Software IRQ from platform
  +--------+---------+
           |
           v
  +------------------+
  | Driver ISR       |     vcam_isr()
  | 1. Read STATUS   |     status = ioread32(INT_STATUS)
  | 2. Ack bits      |     iowrite32(status, INT_STATUS)
  | 3. Schedule work |     schedule_work(&priv->frame_work)
  +------------------+
```

### 5.5 ISR Decision Tree

```
  vcam_isr(irq, priv)
       |
       v
  status = ioread32(INT_STATUS)
       |
       +---> status == 0? ---> return IRQ_NONE (not our interrupt)
       |
       v
  iowrite32(status, INT_STATUS)   // acknowledge all pending bits
       |
       +---> FRAME_DONE? ---> schedule frame completion work
       |
       +---> ERROR?       ---> log error, schedule recovery
       |
       +---> OVERFLOW?    ---> stats.dropped++, log warning
       |
       v
  return IRQ_HANDLED
```

---

## 6. Buffer Descriptor Ring

### 6.1 Ring Registers

The descriptor ring is a circular array in kernel memory.  The hardware
and driver coordinate through HEAD and TAIL pointers.

```
  BUF_RING_ADDR (0x100) -- kernel virtual address of descriptor array
  BUF_RING_SIZE (0x104) -- number of descriptors (max 16)
  BUF_RING_HEAD (0x108) -- driver write pointer (next slot to fill)
  BUF_RING_TAIL (0x10C) -- hardware read pointer (next slot to consume)
```

**HEAD** is written by the driver after submitting a new buffer.
**TAIL** is advanced by hardware after completing a frame.

### 6.2 Descriptor Format

Each descriptor is 16 bytes (4 x 32-bit words):

```
  Byte Offset    Field        Description
  +-----------+--------------------------------------------------+
  |  0x00     |  addr_lo     Buffer address (low 32 bits)        |
  +-----------+--------------------------------------------------+
  |  0x04     |  addr_hi     Buffer address (high 32 bits)       |
  +-----------+--------------------------------------------------+
  |  0x08     |  size        Buffer size in bytes                |
  +-----------+--------------------------------------------------+
  |  0x0C     |  flags       Ownership and status flags          |
  +-----------+--------------------------------------------------+

  sizeof(descriptor) = 16 bytes
```

### 6.3 Descriptor Flag Bits

```
  31   30   29   28               16 15                    0
  +----+----+----+-----------------+------------------------+
  |OWN |DONE|ERR |  Reserved (0)   |    SEQ (sequence #)    |
  +----+----+----+-----------------+------------------------+
    |    |    |
    |    |    +-- Bit 29: ERROR     Error during frame capture
    |    +------- Bit 30: DONE      Frame has been written to buffer
    +------------ Bit 31: OWN       1 = hardware owns this descriptor
```

| Bit   | Name    | Description                                      |
|-------|---------|--------------------------------------------------|
| 31    | OWN     | 1 = hardware owns, 0 = driver owns               |
| 30    | DONE    | Frame has been written to buffer                  |
| 29    | ERROR   | Error during frame capture                        |
| 15:0  | SEQ     | Sequence number (set by hardware)                 |

### 6.4 Ownership Handshake

```
  Driver                              Hardware
  ------                              --------
  Fill desc (addr_lo, addr_hi, size)
  Set OWN flag (bit 31)
  Advance HEAD register
  iowrite32(HEAD, BUF_RING_HEAD)
                                      Detect HEAD != TAIL
                                      Read desc at TAIL
                                      Check OWN flag set
                                      Generate frame into buffer
                                      Clear OWN, set DONE + SEQ
                                      Advance TAIL register
                                      Fire FRAME_DONE interrupt
  ISR: read INT_STATUS
  ISR: ack INT_STATUS (W1C)
  ISR: schedule work
  Work: read BUF_RING_TAIL
  Work: walk from saved_tail to TAIL
  For each DONE desc:
    Set VB2 timestamp + sequence
    Call vb2_buffer_done(VB2_BUF_STATE_DONE)
  Update saved_tail
```

---

## 7. Frame Capture Operation

### 7.1 Ring Setup (start_streaming)

```
  start_streaming():
       |
       v
  1. kcalloc(ring_size, sizeof(descriptor))     // allocate ring
       |
  2. kcalloc(ring_size, sizeof(vb2_buf *))      // tracking array
       |
  3. vcam_hw_set_buf_ring(ring_va, count)       // register with HW
       |
  4. Program format registers:
       iowrite32(width,     FMT_WIDTH)
       iowrite32(height,    FMT_HEIGHT)
       iowrite32(pixfmt,    FMT_PIXFMT)
       iowrite32(stride,    FMT_STRIDE)
       iowrite32(framesize, FMT_FRAMESIZE)
       |
  5. Unmask FRAME_DONE interrupt:
       iowrite32(~FRAME_DONE, INT_MASK)         // 0xFFFFFFFE
       |
  6. Enable streaming:
       iowrite32(ENABLE | STREAM_ON | RING_ENABLE, CTRL)
       |
  7. vcam_hw_notify_ctrl()                      // kick hardware timer
       |
       v
  [Hardware begins generating frames at ~30 fps]
```

### 7.2 Buffer Submission (buf_queue)

```
  buf_queue(vb2_buffer):
       |
       v
  1. Get buffer virtual address from VB2:
       vaddr = vb2_plane_vaddr(vb, 0)
       |
  2. Fill descriptor at ring[HEAD]:
       desc.addr_lo = lower_32_bits(vaddr)
       desc.addr_hi = upper_32_bits(vaddr)
       desc.size    = FMT_FRAMESIZE
       desc.flags   = OWN (bit 31)
       |
  3. Store VB2 buffer pointer in tracking array:
       buf_tracking[HEAD] = vb2_buffer
       |
  4. Advance HEAD:
       HEAD = (HEAD + 1) % ring_size
       |
  5. Write HEAD to doorbell register:
       iowrite32(HEAD, BUF_RING_HEAD)
```

### 7.3 Frame Generation (hardware internal)

```
  Hardware timer fires (~33ms, ~30 fps):
       |
  1. Check CTRL bits:
       ENABLE, STREAM_ON, RING_ENABLE all set?
       |  No --> skip, wait for next timer
       v
  2. Compare HEAD and TAIL:
       HEAD == TAIL? --> ring empty, skip
       |
  3. Read descriptor at ring[TAIL]:
       Verify OWN flag (bit 31) is set
       |
  4. Read format registers:
       width, height, stride from FMT_*
       |
  5. Read image controls:
       BRIGHTNESS (0x040), HFLIP (0x044)
       |
  6. Generate 8-bar color pattern into buffer:
       - Apply brightness offset
       - Apply horizontal flip
       - Embed frame counter in first 4 bytes
       |
  7. Update descriptor:
       Clear OWN (bit 31)
       Set DONE (bit 30)
       Set SEQ (bits 15:0) = frame counter
       |
  8. Update status registers:
       FRAME_COUNT++
       FRAME_TS_LO/HI = current timestamp
       |
  9. Advance TAIL:
       TAIL = (TAIL + 1) % ring_size
       |
  10. Fire FRAME_DONE interrupt (if unmasked)
```

### 7.4 Frame Completion (ISR + work)

```
  IRQ fires
       |
       v
  vcam_isr():
       status = ioread32(INT_STATUS)
       if (status == 0) return IRQ_NONE
       iowrite32(status, INT_STATUS)     // W1C acknowledge
       if (status & FRAME_DONE)
           schedule_work(&priv->frame_work)
       return IRQ_HANDLED
       |
       v
  frame_work_fn():
       |
  1. Read hardware TAIL:
       hw_tail = ioread32(BUF_RING_TAIL)
       |
  2. Walk from saved_tail to hw_tail:
       while (saved_tail != hw_tail):
           desc = &ring[saved_tail]
           |
  3.     Check descriptor flags:
           if (desc->flags & DONE):
               vb = buf_tracking[saved_tail]
               vb->timestamp = ktime_get_ns()
               vb->sequence  = desc->flags & SEQ_MASK
               vb2_buffer_done(vb, VB2_BUF_STATE_DONE)
           else if (desc->flags & ERROR):
               vb2_buffer_done(vb, VB2_BUF_STATE_ERROR)
           |
  4.     Advance saved_tail:
           saved_tail = (saved_tail + 1) % ring_size
```

### 7.5 Stop Streaming

```
  stop_streaming():
       |
  1. iowrite32(0, CTRL)                         // disable HW
       |
  2. vcam_hw_notify_ctrl()                      // stop HW timer
       |
  3. iowrite32(0xFFFFFFFF, INT_MASK)            // mask all interrupts
       |
  4. cancel_work_sync(&priv->frame_work)        // drain pending work
       |
  5. Return pending VB2 buffers:
       for each queued buffer:
           vb2_buffer_done(vb, VB2_BUF_STATE_ERROR)
       |
  6. vcam_hw_clear_buf_ring()                   // unregister ring
       |
  7. kfree(ring), kfree(buf_tracking)           // free memory
```

---

## 8. Ring State Diagrams

### 8.1 Empty Ring

```
  HEAD=0, TAIL=0: Ring empty (no buffers submitted)

  +---------+---------+---------+---------+---------+---------+
  |  empty  |  empty  |  empty  |  empty  |  empty  |  empty  |
  +---------+---------+---------+---------+---------+---------+
    T=0, H=0
```

### 8.2 After Driver Submits 3 Buffers

```
  +---------+---------+---------+---------+---------+---------+
  |   OWN   |   OWN   |   OWN   |  empty  |  empty  |  empty  |
  +---------+---------+---------+---------+---------+---------+
    T=0                            H=3

  Driver filled slots 0, 1, 2 with descriptors (OWN=1)
  and wrote HEAD=3 to BUF_RING_HEAD register.
```

### 8.3 After Hardware Processes 2 Frames

```
  +---------+---------+---------+---------+---------+---------+
  |  DONE   |  DONE   |   OWN   |  empty  |  empty  |  empty  |
  +---------+---------+---------+---------+---------+---------+
                         T=2      H=3

  Hardware consumed slots 0 and 1:
    - Wrote frame data into each buffer
    - Cleared OWN, set DONE + SEQ in flags
    - Advanced TAIL to 2
    - Fired FRAME_DONE interrupt (twice or batched)
```

### 8.4 After Driver Completes 2 Buffers and Resubmits

```
  +---------+---------+---------+---------+---------+---------+
  |   OWN   |   OWN   |   OWN   |  empty  |  empty  |  empty  |
  +---------+---------+---------+---------+---------+---------+
                         T=2                           H=5

  Driver walked ring, completed VB2 buffers for slots 0 and 1.
  User requeued buffers, driver filled slots 3 and 4.
  HEAD advanced to 5.
```

### 8.5 Wrap-Around

```
  +---------+---------+---------+---------+---------+---------+
  |   OWN   |  empty  |  empty  |  empty  |  DONE   |  DONE   |
  +---------+---------+---------+---------+---------+---------+
               H=1                          T=4

  TAIL wraps around past end of ring back to beginning.
  Modular arithmetic: index = pointer % ring_size
```

### 8.6 Ring Full Condition

```
  Ring is full when: (HEAD + 1) % ring_size == TAIL

  All slots contain OWN descriptors -- hardware is processing them.
  Driver cannot submit more buffers until FRAME_DONE frees slots.
  If hardware generates a frame with no available slot: OVERFLOW interrupt.
```

---

## 9. Platform Interface (Part 3)

| Function                   | Purpose                                  |
|----------------------------|------------------------------------------|
| `vcam_hw_map_regs()`       | Get __iomem pointer to register file     |
| `vcam_hw_unmap_regs()`     | Release register mapping                 |
| `vcam_hw_get_irq()`        | Get software IRQ number for request_irq  |
| `vcam_hw_set_buf_ring()`   | Register descriptor ring with hardware   |
| `vcam_hw_clear_buf_ring()` | Tear down descriptor ring                |
| `vcam_hw_notify_ctrl()`    | Notify HW of CTRL register write         |
| `vcam_hw_get_pdev()`       | Get platform_device for dev_info logging |

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

---

## Appendix A: Quick Reference Card

```
Interrupt Registers:
  INT_MASK   (0x008, R/W) -- 1=disabled, 0=enabled
  INT_STATUS (0x00C, W1C) -- FRAME_DONE=bit0, ERROR=bit1, OVERFLOW=bit2

Ring Registers:
  BUF_RING_ADDR (0x100, R/W) -- ring base address
  BUF_RING_SIZE (0x104, R/W) -- number of entries (max 16)
  BUF_RING_HEAD (0x108, R/W) -- driver write pointer (doorbell)
  BUF_RING_TAIL (0x10C, R)   -- hardware read pointer

Descriptor (16 bytes):
  Word 0: addr_lo   (buffer address, low 32 bits)
  Word 1: addr_hi   (buffer address, high 32 bits)
  Word 2: size      (buffer size in bytes)
  Word 3: flags     (OWN=bit31, DONE=bit30, ERROR=bit29, SEQ=bits15:0)

Start streaming:
  kcalloc ring -> vcam_hw_set_buf_ring -> program FMT regs
  -> unmask FRAME_DONE -> write ENABLE|STREAM_ON|RING_ENABLE
  -> vcam_hw_notify_ctrl

Stop streaming:
  write CTRL=0 -> notify -> mask all IRQs -> cancel_work_sync
  -> return buffers -> clear ring -> kfree

Buffer submission (buf_queue):
  fill desc -> set OWN -> advance HEAD -> write BUF_RING_HEAD

Frame completion (ISR + work):
  ISR: read INT_STATUS, ack W1C, schedule work
  Work: read TAIL, walk ring, vb2_buffer_done for DONE descs
```

---

*VCAM-2000 Streaming Datasheet -- Part 3: Interrupts, Descriptor Ring & Frame Capture*
*For use with the V4L2 Camera Driver Curriculum*
