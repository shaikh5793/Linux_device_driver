# Part 1: V4L2 Userspace Foundations

Userspace programs that exercise the core V4L2 ioctls -- one concept per file.
Run these against any V4L2 device (USB camera, v4l2loopback, or the vcam
driver from Part 2 onwards).

## Examples

| File | Key ioctls | What it teaches |
|------|-----------|-----------------|
| `discover.c` | `QUERYCAP` | Scan /dev/videoN, read driver/card/bus, decode device_caps bitmask |
| `formats.c` | `ENUM_FMT`, `G/S/TRY_FMT`, `ENUM_FRAMESIZES` | Pixel format enumeration, resolution discovery, format negotiation |
| `controls.c` | `QUERYCTRL`, `G_CTRL`, `S_CTRL`, `QUERYMENU` | Control enumeration, reading/writing values, menu items |
| `buffers.c` | `REQBUFS`, `QUERYBUF`, `mmap` | Buffer allocation, kernel-to-userspace mapping (no streaming) |

## Build & Run

```bash
make all

# Scan all devices
./discover

# Query a specific device
./discover /dev/video0
./formats /dev/video0
./controls /dev/video0
./buffers /dev/video0
```

## Prerequisites

- A V4L2 device: USB camera, `v4l2loopback`, or the vcam driver from Part 2
- `linux/videodev2.h` header (install `libv4l-dev` if missing)
- User in the `video` group for device access

## Progression

These four programs cover the userspace side of V4L2. Part 2 switches to
kernel space to implement the driver that responds to these ioctls.

| Concept | Userspace (Part 1) | Kernel (Part 2+) |
|---------|-------------------|-------------------|
| Capabilities | `discover.c` calls QUERYCAP | `vcam_querycap()` responds |
| Formats | `formats.c` calls ENUM_FMT/S_FMT | `vcam_enum_fmt()`, `vcam_s_fmt()` |
| Controls | `controls.c` calls G/S_CTRL | `vcam_s_ctrl()` writes hardware (Part 6) |
| Buffers | `buffers.c` calls REQBUFS/mmap | VB2 framework manages buffers (Part 3) |

---

## V4L2 Concepts

### Introduction to V4L2

V4L2 (Video for Linux, version 2) is the standard Linux kernel API for
video capture and output devices. It sits between user-space applications
and kernel-space drivers that control the actual hardware (webcams, TV tuners,
video encoders, etc.).

When you plug in a USB webcam or load a virtual camera kernel module, the
kernel driver registers one or more "video device nodes" under `/dev`. These
nodes appear as character device files named `/dev/video0`, `/dev/video1`,
etc. Each node represents one functional endpoint of a video device. For
example, a single physical webcam might create two nodes: one for the video
stream (`/dev/video0`) and one for metadata about each frame (`/dev/video1`).
The numbering is assigned dynamically by the kernel in the order devices are
registered -- there is no guarantee that your webcam will always be
`/dev/video0`.

All communication with V4L2 devices follows the standard POSIX file I/O model:

1. `open()` the `/dev/videoN` node to get a file descriptor.
2. Use `ioctl()` calls to negotiate formats, allocate buffers, and control
   device behavior.
3. Use `read()`/`write()` or `mmap()`-based streaming to transfer frames.
4. `close()` the file descriptor when done.

### The videodev2.h Header

`<linux/videodev2.h>` is the header for V4L2 user-space programming. It defines:

- All `VIDIOC_*` ioctl command numbers (`VIDIOC_QUERYCAP`, `VIDIOC_S_FMT`, ...)
- All V4L2 data structures (`struct v4l2_capability`, `struct v4l2_format`, ...)
- All `V4L2_CAP_*` capability bitmask constants
- All `V4L2_PIX_FMT_*` pixel format fourcc codes

This header is part of the kernel's UAPI (User-space API) headers -- the
contract between user-space and the kernel's V4L2 subsystem.

---

## discover.c -- Device Discovery

### VIDIOC_QUERYCAP

The very first ioctl you should issue on any V4L2 device is `VIDIOC_QUERYCAP`
("query capabilities"). This is the V4L2 equivalent of asking a device:
"Who are you, and what can you do?" Every compliant V4L2 driver MUST support
it. If it fails, the device node is either not a V4L2 device at all, or the
driver is broken.

The ioctl calling convention:

```c
ioctl(fd, VIDIOC_QUERYCAP, &cap)
```

- `fd` -- file descriptor from `open()`
- `VIDIOC_QUERYCAP` -- the ioctl command number (defined in videodev2.h)
- `&cap` -- pointer to `struct v4l2_capability` the kernel will fill in

V4L2 ioctls follow a naming pattern:

| Prefix | Meaning |
|--------|---------|
| `VIDIOC_QUERYCAP` | Query (read-only, kernel fills struct) |
| `VIDIOC_G_FMT` | Get (read a current setting) |
| `VIDIOC_S_FMT` | Set (write a new setting, kernel may adjust) |
| `VIDIOC_REQBUFS` | Request (allocate resources) |

### struct v4l2_capability

QUERYCAP returns a `struct v4l2_capability`:

- **`driver[16]`** -- The kernel driver name (e.g., `"uvcvideo"`, `"vivid"`,
  `"bcm2835-v4l2"`). Applications can use this for driver-specific workarounds.

- **`card[32]`** -- Human-readable device description (e.g., `"HD Pro Webcam C920"`).
  This is what you show in a device selection menu.

- **`bus_info[32]`** -- Physical location or bus attachment (e.g.,
  `"usb-0000:00:14.0-1"` for USB, `"platform:camera0"` for SoC). Useful
  for distinguishing two identical cameras on different ports.

- **`version`** -- Driver version encoded as a 32-bit integer via
  `KERNEL_VERSION(major, minor, patch)`. Bits 16-23 = major, 8-15 = minor,
  0-7 = patch.

- **`capabilities`** -- Bitmask of ALL capabilities across ALL device nodes
  the driver creates.

- **`device_caps`** -- Bitmask of capabilities for THIS SPECIFIC device node.
  **Always use `device_caps`** to decide what operations are valid on the fd
  you opened. If you only look at `capabilities`, you might think a metadata
  node supports video capture and get confusing errors.

### Capability Bits

| Bit | Meaning |
|-----|---------|
| `V4L2_CAP_VIDEO_CAPTURE` | Captures single-planar video frames. Most common -- USB webcams, laptop cameras, TV tuners. |
| `V4L2_CAP_VIDEO_OUTPUT` | Accepts frames and sends them somewhere (display overlay, encoder, loopback). |
| `V4L2_CAP_VIDEO_OVERLAY` | Overlays video directly onto the framebuffer. Legacy, rarely used today. |
| `V4L2_CAP_VIDEO_CAPTURE_MPLANE` | Multi-planar capture. Separate buffers for Y and UV planes. Common on SoC ISPs. Uses `v4l2_pix_format_mplane` structs. |
| `V4L2_CAP_VIDEO_OUTPUT_MPLANE` | Multi-planar output. |
| `V4L2_CAP_VIDEO_M2M` | Memory-to-memory: has both input (OUTPUT) and output (CAPTURE) queues. Hardware video codecs. |
| `V4L2_CAP_VIDEO_M2M_MPLANE` | M2M with multi-planar buffers. Most modern SoC codecs. |
| `V4L2_CAP_META_CAPTURE` | Produces per-frame metadata (3A statistics, sensor data) rather than pixels. |
| `V4L2_CAP_STREAMING` | Supports streaming I/O (REQBUFS/QBUF/DQBUF/STREAMON). High-performance path. Almost all modern drivers. |
| `V4L2_CAP_READWRITE` | Supports simple `read()`/`write()` I/O. Simpler but slower (kernel copies data). |
| `V4L2_CAP_IO_MC` | Part of a Media Controller pipeline (`/dev/mediaN`). Must configure routing before capture. Common on embedded/mobile. |

### Device Discovery Methods

The program scans `/dev/video0` through `/dev/video63` and issues QUERYCAP on
each. V4L2 does not provide a single syscall to list all video devices; the
`/dev/` nodes are the discovery mechanism.

Alternative approaches (not demonstrated):

- Enumerate `/sys/class/video4linux/` for sysfs entries
- Use libudev to monitor hotplug events
- Use the Media Controller API (`/dev/mediaN`) for complex pipelines

Device numbers are assigned dynamically and are **not stable** across reboots.
For stable identification, use udev rules based on serial number or USB port path.

---

## formats.c -- Format Negotiation

### What is Format Negotiation?

Every video device can only produce frames in certain pixel layouts and at
certain resolutions. The application must discover what the hardware supports,
then agree on a configuration. This discovery-and-agreement process follows
a well-defined ioctl sequence:

1. **ENUMERATE** (`VIDIOC_ENUM_FMT`, `VIDIOC_ENUM_FRAMESIZES`) -- ask the
   driver what pixel formats and resolutions it supports.
2. **TRY** (`VIDIOC_TRY_FMT`) -- propose a combination and see what the
   driver would actually give you. Does NOT change device state.
3. **SET** (`VIDIOC_S_FMT`) -- commit a format to the hardware. The driver
   may adjust the requested values.
4. **VERIFY** (`VIDIOC_G_FMT`) -- read back the currently active format.

Correct format negotiation is critical because once streaming starts, every
frame will be in exactly the negotiated format. A mismatch means corrupted
data or buffer overflows.

### FOURCC Codes

A "FOURCC" (Four Character Code) is a 32-bit identifier made by packing four
ASCII characters into a `uint32_t`. V4L2 uses FOURCC codes as the universal
way to name pixel formats:

```c
#define v4l2_fourcc(a, b, c, d) \
    ((__u32)(a) | ((__u32)(b) << 8) | \
     ((__u32)(c) << 16) | ((__u32)(d) << 24))
```

Common FOURCC codes:

| FOURCC | V4L2 Constant | BPP | Description |
|--------|---------------|-----|-------------|
| `YUYV` | `V4L2_PIX_FMT_YUYV` | 2 | Packed YUV 4:2:2. Most common uncompressed webcam format. |
| `MJPG` | `V4L2_PIX_FMT_MJPEG` | varies | Motion JPEG. Common for USB webcams at high resolutions. |
| `RGB3` | `V4L2_PIX_FMT_RGB24` | 3 | Packed RGB, 3 bytes/pixel. Simple but rarely native to sensors. |
| `NV12` | `V4L2_PIX_FMT_NV12` | 1.5 | Semi-planar YUV 4:2:0. Standard for hardware decoders and ARM SoC display pipelines. |

Why pack four characters into a u32? Fixed size (no pointers), fast comparison
(single integer compare), kernel-friendly (no dynamic memory), and
human-readable when printed.

### VIDIOC_ENUM_FMT

Iterates over every pixel format the driver supports for a given buffer type.
Fill in `struct v4l2_fmtdesc` with `.type` and `.index = 0, 1, 2, ...`,
call the ioctl. When the index exceeds the last format, the ioctl returns
`-EINVAL`. This "increment until EINVAL" pattern is used throughout V4L2.

Key returned fields:

- `.description` -- human-readable name (e.g., "YUV 4:2:2 (YUYV)")
- `.pixelformat` -- the FOURCC code
- `.flags` -- `V4L2_FMT_FLAG_COMPRESSED` (MJPEG, H.264) or
  `V4L2_FMT_FLAG_EMULATED` (software-converted, costs CPU)

### VIDIOC_ENUM_FRAMESIZES

For a given pixel format, enumerates supported resolutions. Frame sizes are
per-format because hardware constraints differ (e.g., a sensor might support
1080p in YUYV but only 4K in MJPEG due to USB bandwidth).

Three types of frame size descriptions:

- **DISCRETE** -- Fixed list of resolutions. Most common for USB webcams.
- **STEPWISE** -- Range with min/max and step increments per dimension.
- **CONTINUOUS** -- Like stepwise but step = 1 in both dimensions. Rare.

### struct v4l2_pix_format

The pixel format parameters inside `struct v4l2_format`:

- **`width`**, **`height`** -- frame dimensions in pixels
- **`pixelformat`** -- FOURCC code
- **`bytesperline`** -- stride (bytes per row). May be larger than
  `width * bpp` due to hardware alignment. **Always use this** for row offset
  calculations.
- **`sizeimage`** -- total frame size in bytes. For uncompressed:
  `bytesperline * height`. For compressed: maximum buffer size needed.
- **`field`** -- interlace mode. `V4L2_FIELD_NONE` = progressive (modern
  sensors). `V4L2_FIELD_INTERLACED` = traditional TV.
- **`colorspace`** -- color encoding (`V4L2_COLORSPACE_SRGB`,
  `V4L2_COLORSPACE_REC709`, `V4L2_COLORSPACE_JPEG`).

### G_FMT, TRY_FMT, S_FMT

- **`VIDIOC_G_FMT`** -- reads the currently active format. Set `.type`,
  driver fills everything else.

- **`VIDIOC_TRY_FMT`** -- safe sandbox. Fill in desired values, driver
  adjusts to the nearest valid combination. Does NOT change device state.
  Safe to call at any time, even while streaming.

- **`VIDIOC_S_FMT`** -- like TRY_FMT but also applies the result to the
  hardware. Two crucial rules:
  1. **Always check the returned struct.** Never assume the driver accepted
     your exact request.
  2. **Fails with EBUSY during streaming.** Must STREAMOFF, free buffers,
     set new format, re-allocate, and restart.

### Typical Application Sequence

1. `open()` the device
2. `VIDIOC_QUERYCAP` -- verify VIDEO_CAPTURE support
3. `VIDIOC_ENUM_FMT` + `VIDIOC_ENUM_FRAMESIZES` -- discover capabilities
4. `VIDIOC_TRY_FMT` -- probe preferred format
5. `VIDIOC_S_FMT` -- commit; read back actual values
6. `VIDIOC_REQBUFS` -- allocate buffers using `sizeimage` from step 5
7. `VIDIOC_QBUF` + `VIDIOC_STREAMON` -- start capture
8. `poll()`/`select()` + `VIDIOC_DQBUF` -- dequeue frames
9. Process/display each frame
10. `VIDIOC_STREAMOFF` + `close()`

---

## buffers.c -- Buffer Management and MMAP

### Why Video Devices Need Special Buffer Management

Video frames are enormous. A single uncompressed 1080p RGB24 frame:

    1920 x 1080 x 3 = 6,220,800 bytes (~6 MB)

At 30 fps, that is ~180 MB/s. Even 640x480 RGB24 at 30 fps is ~27 MB/s.
Using `read()`/`write()` would copy every frame from kernel to userspace --
pure waste. The solution is **zero-copy** buffer sharing: the driver fills a
buffer, and userspace reads directly from the same physical memory through a
virtual mapping.

A second problem is pipeline overlap. With only one buffer, the hardware must
wait while userspace processes. With multiple buffers (typically 3-5), the
hardware fills buffer N+1 while userspace reads buffer N.

### Memory Modes

V4L2 supports three memory modes:

**V4L2_MEMORY_MMAP** -- The kernel allocates buffers (via VB2), userspace
maps them with `mmap()`. Most common and safest:
- Driver controls alignment and placement (DMA constraints always satisfied)
- Can allocate from CMA when hardware requires contiguous memory
- Works on every driver and architecture
- Almost every V4L2 tool (v4l2-ctl, FFmpeg, GStreamer) defaults to MMAP

**V4L2_MEMORY_USERPTR** -- Userspace allocates with `malloc`/`posix_memalign`,
passes pointer to kernel. Serious practical issues:
- Must meet hardware alignment requirements
- On non-cache-coherent architectures (most ARM), driver must flush/invalidate
  caches on every QBUF/DQBUF
- If hardware needs physically contiguous memory (no IOMMU), malloc fails
- Many drivers do not support it. Rarely used in modern applications.

**V4L2_MEMORY_DMABUF** -- Buffers are dma-buf file descriptors shared between
devices. A camera can export capture buffers as fds; a GPU or display can
import them for true zero-copy across hardware. Essential in embedded pipelines
(camera -> ISP -> GPU -> display).

### VIDIOC_REQBUFS

Tells the kernel: "Allocate N buffers for VIDEO_CAPTURE in MMAP mode."

Key fields of `struct v4l2_requestbuffers`:

- `count` -- how many buffers to allocate. The driver may return more or fewer.
  **Always check `count` after the ioctl.**
- `type` -- buffer type (`V4L2_BUF_TYPE_VIDEO_CAPTURE`)
- `memory` -- memory model (`V4L2_MEMORY_MMAP`)

Calling REQBUFS with `count=0` **frees** all previously allocated buffers.

Under the hood, REQBUFS goes through VB2 (videobuf2), which calls the driver's
`queue_setup()` callback and allocates via vb2-vmalloc, vb2-dma-contig, or
vb2-dma-sg.

### VIDIOC_QUERYBUF

After REQBUFS, userspace needs two pieces of information per buffer to map it:

- `buf.length` -- buffer size in bytes
- `buf.m.offset` -- opaque cookie to pass to `mmap()` so the kernel knows
  which buffer to map

Other returned fields:
- `buf.flags` -- state flags (QUEUED, DONE, etc.)
- `buf.bytesused` -- bytes of valid data (0 for unfilled buffers)

### mmap() for V4L2 Buffers

```c
ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
```

| Argument | Value | Why |
|----------|-------|-----|
| `addr` | `NULL` | Let kernel choose the virtual address |
| `length` | `buf.length` | From QUERYBUF |
| `prot` | `PROT_READ \| PROT_WRITE` | Read captured pixels; some drivers require RW |
| `flags` | `MAP_SHARED` | **Critical.** DMA writes must be visible to userspace. MAP_PRIVATE would defeat zero-copy. |
| `fd` | video device fd | Driver's mmap handler finds the right buffer queue |
| `offset` | `buf.m.offset` | Opaque token identifying which buffer to map |

### The Streaming Lifecycle

```
REQBUFS              Allocate N buffers
    |
QUERYBUF + mmap      Map each buffer to userspace
    |
QBUF (x N)           Queue all buffers to driver
    |
STREAMON              Start hardware capture
    |
+---------------------------+
| poll() / select()         |   Wait for a filled buffer
|       |                   |
| DQBUF                     |   Dequeue (belongs to userspace now)
|       |                   |
| Process the frame         |   Read pixels at buffers[i].start
|       |                   |
| QBUF                      |   Re-queue to driver
+---------------------------+
    |  (repeat)
STREAMOFF             Stop capture
    |
munmap (x N)          Unmap all buffers
    |
REQBUFS(count=0)      Free buffer memory
    |
close(fd)
```

Buffers cycle between two owners:
- **QUEUED** (after QBUF) -- belongs to the DRIVER. Hardware may DMA into it.
  Userspace must NOT touch it.
- **DEQUEUED** (after DQBUF) -- belongs to USERSPACE. Driver will not touch it.

Safe cleanup order: `STREAMOFF -> munmap -> REQBUFS(0) -> close`.

---

## controls.c -- V4L2 Controls

### What Are V4L2 Controls?

Controls are the tunable parameters ("knobs") of a video device: brightness,
contrast, saturation, gain, exposure, white balance, gamma, etc. Each gets a
unique 32-bit Control ID (CID), e.g.:

- `V4L2_CID_BRIGHTNESS` = `0x00980900`
- `V4L2_CID_CONTRAST` = `0x00980901`
- `V4L2_CID_EXPOSURE` = `0x009a0901` (camera class)

### How Controls Map to Hardware

Under the hood, the driver translates each control into hardware writes:

- **Raw sensor** (OV5640, IMX219): I2C register writes. Setting
  `V4L2_CID_EXPOSURE` might write a 16-bit value across registers
  `0x3500`/`0x3501`.
- **UVC webcam**: USB control transfer to the Processing Unit or Camera
  Terminal, per the UVC specification.
- **ISP-based devices**: Writes to ISP register blocks or firmware mailboxes.

From userspace, all this is hidden behind the same four ioctls.

### Control Classes

| Class | CID Range | Examples |
|-------|-----------|----------|
| `V4L2_CTRL_CLASS_USER` | `0x0098xxxx` | Brightness, contrast, saturation, hue, audio volume |
| `V4L2_CTRL_CLASS_CAMERA` | `0x009Axxxx` | Exposure, focus, zoom, pan, tilt, iris |
| `V4L2_CTRL_CLASS_MPEG` | `0x0099xxxx` | Bitrate, GOP size, profile, level |
| `V4L2_CTRL_CLASS_IMAGE_SOURCE` | `0x009Exxxx` | Analog/digital gain, blanking, link frequency |
| `V4L2_CTRL_CLASS_IMAGE_PROC` | `0x009Fxxxx` | Color correction, lens shading, test patterns |

### Control Types

| Type | Description |
|------|-------------|
| `INTEGER` | Numeric value within [min..max] advancing in steps. |
| `BOOLEAN` | On/off toggle (0 or 1). |
| `MENU` | Named options (index into a list). Use QUERYMENU to discover entries. |
| `BUTTON` | One-shot trigger, no persistent value. Writing causes an action. |
| `INTEGER64` | Signed 64-bit integer (rare). |
| `STRING` | NUL-terminated string. Requires extended control API. |
| `BITMASK` | 32-bit bitmask with driver-defined bit meanings. |
| `INTEGER_MENU` | Like MENU but entries are 64-bit integer values (e.g., link frequencies in Hz). |

### Control Flags

| Flag | Meaning |
|------|---------|
| `DISABLED` | Not available on this hardware/configuration. Treat as non-existent. |
| `READ_ONLY` | Can read with G_CTRL but cannot write. |
| `WRITE_ONLY` | Can write but reading is not meaningful. |
| `GRABBED` | Another application holds exclusive control. Writes fail with EBUSY. |
| `VOLATILE` | Value changes without userspace writing (e.g., auto-exposure continuously updates the exposure value). |
| `INACTIVE` | Exists but has no effect because another control overrides it (e.g., manual white balance while auto is ON). |

### VIDIOC_QUERYCTRL

Discovers whether a control exists and its valid range. Walk the loop from
`V4L2_CID_BASE` to `V4L2_CID_LASTP1`, calling QUERYCTRL for each ID.
Returns `-EINVAL` for unsupported controls.

Returns: `.name`, `.type`, `.minimum`, `.maximum`, `.step`,
`.default_value`, `.flags`.

For non-user classes (camera, MPEG), use the `V4L2_CTRL_FLAG_NEXT_CTRL`
mechanism for efficient enumeration of sparse ID spaces.

### VIDIOC_QUERYMENU

For menu-type controls, iterates over named options. Fill in `.id` and
`.index`, call the ioctl. Not every index between min and max is guaranteed
valid -- some drivers leave gaps. The ioctl returns `-EINVAL` for unavailable
indices.

### VIDIOC_G_CTRL and VIDIOC_S_CTRL

**G_CTRL** reads a control value. Fill in `.id`, call the ioctl, read `.value`.
Caveats: meaningless for BUTTON controls, fails with EACCES for WRITE_ONLY,
and VOLATILE controls may change between consecutive reads.

**S_CTRL** writes a new value. The driver may:
1. Clamp or round to the nearest valid step
2. Apply immediately, at the next frame boundary, or at stream-on time
   (driver-dependent)

**The read-modify-verify pattern:**
1. READ current value with G_CTRL (save for restoration)
2. WRITE new value with S_CTRL
3. READ BACK with G_CTRL to verify what the driver actually applied

Always verify -- never assume the driver accepted your exact value.
