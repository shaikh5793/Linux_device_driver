# VCAM-2000 Virtual Image Sensor Controller -- DMA-Buf Datasheet
## Part 5: DMA-Buf Export & Buffer Sharing

Copyright (c) 2024 TECH VEDA / Author: Raghu Bharadwaj

**Applicable to**: Part 5 (VIDIOC_EXPBUF, dma-buf export, buf_reader importer)

This document describes how the VCAM-2000 driver exports VB2 frame buffers
as dma-buf file descriptors and how an importer module consumes them.
The hardware register interface is unchanged from Part 3; this datasheet
focuses on the buffer sharing mechanism built on top of the existing
descriptor ring.

---

## 1. Overview

Part 5 adds dma-buf export capability to the VCAM-2000 driver.  This allows
userspace (or another kernel module) to obtain a file descriptor representing
a VB2 frame buffer, which can then be imported and memory-mapped by a
consumer.

The hardware does not change -- the same descriptor ring, interrupt controller,
and frame generation pipeline from Part 3 are used.  The new functionality
is entirely in the V4L2/VB2 layer.

```
  +-----------------+       +------------------+       +------------------+
  | VCAM-2000       |       | dma-buf          |       | buf_reader       |
  | V4L2 Driver     |       | Framework        |       | (importer)       |
  |                 |       |                  |       |                  |
  | VB2 queue       |       |                  |       |                  |
  |   |             |       |                  |       |                  |
  |   +--EXPBUF---->|------>| dma_buf fd       |------>| import fd        |
  |   |             |       | (file descriptor)|       | dma_buf_attach   |
  |   | buf_queue   |       |                  |       | dma_buf_map      |
  |   |   |         |       |                  |       | read pixels      |
  |   |   v         |       |                  |       | dma_buf_unmap    |
  |   | descriptor  |       |                  |       | dma_buf_detach   |
  |   | ring        |       |                  |       |                  |
  |   |   |         |       |                  |       |                  |
  |   |   v         |       |                  |       |                  |
  |   | HW writes   |       |                  |       |                  |
  |   | frame data  |       |                  |       |                  |
  +-----------------+       +------------------+       +------------------+
```

---

## 2. EXPBUF and the Descriptor Ring

### 2.1 How EXPBUF Relates to the Ring

The `VIDIOC_EXPBUF` ioctl exports a VB2 buffer as a dma-buf file descriptor.
The exported buffer is the **same memory** that the descriptor ring points to.
No copy is made.

```
  VB2 Buffer Pool
  +---------+---------+---------+---------+
  | buf[0]  | buf[1]  | buf[2]  | buf[3]  |
  +---------+---------+---------+---------+
       |         |         |         |
       |    EXPBUF(index=1)|         |
       |         |         |         |
       |         v         |         |
       |    dma-buf fd=7   |         |
       |         |         |         |
       v         v         v         v
  Descriptor Ring (same buffers)
  +-------+-------+-------+-------+
  |desc[0]|desc[1]|desc[2]|desc[3]|
  | addr= | addr= | addr= | addr= |
  |buf[0] |buf[1] |buf[2] |buf[3] |
  +-------+-------+-------+-------+
       ^                       ^
       TAIL                    HEAD
```

The exported dma-buf fd references the same physical memory that the
hardware writes frame data into via the descriptor ring.

### 2.2 Buffer Lifecycle with EXPBUF

```
  1. VIDIOC_REQBUFS(count=4)       -- VB2 allocates 4 frame buffers
  2. VIDIOC_EXPBUF(index=N)        -- export buffer N as dma-buf fd
  3. VIDIOC_QBUF(index=N)          -- queue buffer for capture
  4. VIDIOC_STREAMON                -- start streaming
       |
       v
  [Hardware writes frame into buffer N via descriptor ring]
       |
       v
  5. VIDIOC_DQBUF                  -- dequeue completed buffer
  6. Importer reads buffer via dma-buf fd
  7. VIDIOC_QBUF(index=N)          -- requeue for next frame
       |
       v
  [Repeat from step 4]
```

---

## 3. The buf_reader Importer Pattern

The `buf_reader` is a kernel module that demonstrates the importer side of
dma-buf sharing.  It receives a dma-buf file descriptor, attaches to the
buffer, maps it, and reads pixel data.

### 3.1 Import Flow

```
  buf_reader module                     dma-buf / VB2 exporter
  -------------------                   ----------------------

  1. dma_buf_get(fd)
       |
       v                                Returns struct dma_buf *
  2. dma_buf_attach(dmabuf, dev)
       |
       v                                Creates attachment
  3. dma_buf_map_attachment(att,
       DMA_FROM_DEVICE)
       |
       v                                Returns sg_table
  4. Access buffer via sg_table
     or vmap:
       vaddr = dma_buf_vmap(dmabuf)
       read pixels at vaddr
       dma_buf_vunmap(dmabuf, vaddr)
       |
  5. dma_buf_unmap_attachment(att, sgt)
       |
  6. dma_buf_detach(dmabuf, att)
       |
  7. dma_buf_put(dmabuf)
```

### 3.2 Synchronization

The importer must coordinate with the V4L2 capture cycle.  The buffer
should only be read after `VIDIOC_DQBUF` returns it (meaning hardware
has finished writing the frame via the descriptor ring and the DONE flag
is set in the descriptor).

```
  UNSAFE:  Reading buffer while OWN flag is set (hardware writing)
  SAFE:    Reading buffer after DQBUF (DONE flag set, hardware finished)

  Timeline:
  QBUF ----[OWN: HW writing]---- FRAME_DONE ---- DQBUF ----[SAFE: read]---- QBUF
```

---

## 4. VB2 Memory Types

Part 5 uses `VB2_MEMORY_MMAP` for buffer allocation.  The VB2 framework
manages the underlying memory.  `VIDIOC_EXPBUF` wraps this memory in a
dma-buf for sharing.

```
  VB2 allocator (MMAP)
       |
       v
  Kernel pages (VB2-managed)
       |
       +-- vb2_plane_vaddr() --> driver writes descriptor addr
       |
       +-- VIDIOC_EXPBUF    --> dma-buf fd for importer
       |
       +-- VIDIOC_MMAP      --> userspace mapping (alternative)
```

---

## 5. Hardware Register Reference

No new hardware registers are introduced in Part 5.  The complete register
set used for streaming is documented in the Part 3 datasheet:

- **VCAM-2000-Streaming-Datasheet.md** (Part 3) -- descriptor ring,
  interrupts, format registers, CTRL/STATUS

The descriptor ring, FRAME_DONE interrupt, and buffer completion flow
are identical to Part 3.  The only addition is the VB2/dma-buf export
layer on top.

---

## 6. Key Differences from Part 3

| Aspect              | Part 3                    | Part 5                         |
|---------------------|---------------------------|--------------------------------|
| Buffer access       | Userspace via MMAP only   | MMAP + dma-buf fd export       |
| VIDIOC_EXPBUF       | Not implemented           | Implemented                    |
| Importer module     | None                      | buf_reader demonstrates import |
| Hardware changes    | --                        | None (same descriptor ring)    |
| Descriptor ring     | Same                      | Same                           |
| Interrupt handling  | Same                      | Same                           |

---

## Appendix A: Quick Reference Card

```
EXPBUF:
  VIDIOC_EXPBUF(index=N) --> returns dma-buf fd
  Exported buffer = same memory as descriptor ring buffer[N]
  No copy -- zero-copy sharing

Importer (buf_reader) sequence:
  dma_buf_get(fd)
  dma_buf_attach(dmabuf, dev)
  dma_buf_map_attachment(att, DMA_FROM_DEVICE)
  vaddr = dma_buf_vmap(dmabuf)
  [read pixel data]
  dma_buf_vunmap(dmabuf, vaddr)
  dma_buf_unmap_attachment(att, sgt)
  dma_buf_detach(dmabuf, att)
  dma_buf_put(dmabuf)

Synchronization:
  Only read after VIDIOC_DQBUF (buffer completed by hardware)
  Never read while OWN flag is set in descriptor

Hardware:
  No new registers -- see Part 3 streaming datasheet
  Same descriptor ring, same interrupts, same frame capture flow
```

---

*VCAM-2000 DMA-Buf Datasheet -- Part 5: DMA-Buf Export & Buffer Sharing*
*For use with the V4L2 Camera Driver Curriculum*
