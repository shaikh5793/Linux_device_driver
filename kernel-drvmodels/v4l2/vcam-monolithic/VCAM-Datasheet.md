# VCAM — Virtual V4L2 Camera Device Reference

## Document Scope

This datasheet describes the V4L2 interface and buffer formats for:
- **vcam** — Virtual V4L2 capture device (Parts 2-6)

---

## 1. Device Overview

| Parameter | Value |
|-----------|-------|
| Device Node | `/dev/videoN` |
| V4L2 Type | `V4L2_BUF_TYPE_VIDEO_CAPTURE` (Parts 2-5) |
| | `V4L2_BUF_TYPE_VIDEO_OUTPUT` (Part 6) |
| Driver Name | `vcam` / `vcam_vb2` / `vcam_ctrl` / `vcam_expbuf` / `vout_dmabuf` |
| Bus Info | `platform:vcam` |
| Capabilities | `V4L2_CAP_VIDEO_CAPTURE \| V4L2_CAP_STREAMING` |

---

## 2. Supported Formats

| Index | FourCC | V4L2 Format | BPP | Description |
|-------|--------|-------------|-----|-------------|
| 0 | `RGB3` | `V4L2_PIX_FMT_RGB24` | 3 | 24-bit RGB (R,G,B byte order) |
| 1 | `YUYV` | `V4L2_PIX_FMT_YUYV` | 2 | YUV 4:2:2 packed |

### 2.1 RGB24 Pixel Layout

```
  Byte 0: Red   (R)
  Byte 1: Green (G)
  Byte 2: Blue  (B)

  Note: V4L2 RGB24 (R,G,B memory order) ≠ DRM_FORMAT_RGB888 (B,G,R)
        V4L2 RGB24 = DRM_FORMAT_BGR888
```

### 2.2 YUYV Pixel Layout

```
  Byte 0: Y0  (luma of pixel 0)
  Byte 1: Cb  (blue chroma, shared)
  Byte 2: Y1  (luma of pixel 1)
  Byte 3: Cr  (red chroma, shared)

  Two pixels packed in 4 bytes.
```

---

## 3. Frame Parameters

| Parameter | Value |
|-----------|-------|
| Default Resolution | 640 x 480 |
| Frame Rate | 30 fps (timer-driven, Parts 3+) |
| Colorspace | `V4L2_COLORSPACE_SRGB` |
| Field | `V4L2_FIELD_NONE` (progressive) |

### 3.1 Buffer Size Calculation

```
  bytesperline = width * bpp
  sizeimage    = bytesperline * height

  RGB24:  bytesperline = 640 * 3 = 1920, sizeimage = 921600
  YUYV:   bytesperline = 640 * 2 = 1280, sizeimage = 614400
```

---

## 4. V4L2 Controls (Part 4+)

| CID | Name | Type | Min | Max | Default | Step |
|-----|------|------|-----|-----|---------|------|
| `V4L2_CID_BRIGHTNESS` | Brightness | INTEGER | 0 | 255 | 128 | 1 |
| `V4L2_CID_HFLIP` | Horizontal Flip | BOOLEAN | 0 | 1 | 0 | 1 |

### 4.1 Brightness Effect

Applied to generated test pattern: each RGB component is adjusted by
`(brightness - 128)`, clamped to [0, 255].

### 4.2 Horizontal Flip Effect

When enabled, the test pattern is rendered right-to-left within each scanline.

---

## 5. VB2 Buffer Management (Parts 3+)

### 5.1 Memory Types

| Type | V4L2 Flag | Description |
|------|-----------|-------------|
| MMAP | `V4L2_MEMORY_MMAP` | Kernel-allocated, mmap'd to userspace |
| DMABUF | `V4L2_MEMORY_DMABUF` | External dma-buf fd imported (Part 6) |

### 5.2 Streaming Sequence

```
  1. VIDIOC_REQBUFS   → allocate N buffers (typically 4)
  2. VIDIOC_QUERYBUF  → get buffer info (offset for mmap)
  3. mmap()           → map each buffer to userspace
  4. VIDIOC_QBUF      → queue empty buffers
  5. VIDIOC_STREAMON   → start capture (timer begins)
  6. VIDIOC_DQBUF     → dequeue filled buffer (blocks until ready)
  7. process frame...
  8. VIDIOC_QBUF      → re-queue buffer
  9. goto 6
  10. VIDIOC_STREAMOFF → stop capture
```

---

## 6. DMA-buf Export (Part 5: vcam_expbuf)

### 6.1 VIDIOC_EXPBUF

```c
struct v4l2_exportbuffer {
    __u32 type;     /* V4L2_BUF_TYPE_VIDEO_CAPTURE */
    __u32 index;    /* buffer index (0..N-1) */
    __u32 plane;    /* 0 (single-plane) */
    __u32 flags;    /* O_CLOEXEC | O_RDONLY */
    __s32 fd;       /* [out] dma-buf fd */
};
```

The exported dma-buf fd can be:
- Imported by DRM via `PRIME_FD_TO_HANDLE` (Part 8 of DRM)
- Imported by NPU via `PRIME_FD_TO_HANDLE` (Part 8 of NPU)
- Used for zero-copy frame sharing across subsystems

---

## 7. DMA-buf Import (Part 6: vout_dmabuf)

### 7.1 V4L2 OUTPUT Device

```
  External producer (DMA heap) → dma-buf fd → VIDIOC_QBUF (DMABUF) →
    vout renders frame from imported buffer
```

V4L2 OUTPUT type reverses the capture flow: userspace provides filled
buffers, the driver "displays" (validates and logs) them.

---

## 8. Test Pattern

The virtual camera generates a test pattern with:
- **Color bars:** 8 vertical bars (white, yellow, cyan, green, magenta, red, blue, black)
- **Frame counter:** Incremented per frame, embedded in pixel data
- **Brightness:** Applied uniformly per control setting
- **Flip:** Horizontal mirror when enabled

```
  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
  │White │Yellow│ Cyan │Green │Magenta│ Red  │ Blue │Black │
  │      │      │      │      │      │      │      │      │
  │      │      │      │      │      │      │      │      │
  │      │      │      │      │      │      │      │      │
  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
  640 pixels wide, 480 pixels tall, 30 fps
```
