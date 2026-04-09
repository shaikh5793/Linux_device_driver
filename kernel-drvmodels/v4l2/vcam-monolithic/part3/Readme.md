# Part 3: V4L2 Capture Device with VB2 Streaming (Timer-Based)

Adds VB2 (videobuf2) buffer management and streaming to the capture device.
Frames are delivered via a simple delayed work timer at ~30fps — the simplest
possible streaming model before introducing DMA and interrupts.

**Source**: `vcam_vb2.c` | **Module**: `vcam_vb2.ko`

## Concepts Introduced (over Part 2)

- **VB2 (videobuf2) buffer management** — `vb2_queue` with the five core callbacks
- **queue_setup** — negotiate buffer count and sizes with VB2 framework
- **buf_prepare / buf_queue** — validate buffers and hand them to the driver
- **start_streaming / stop_streaming** — begin/end frame capture
- **Timer-driven frame delivery** — `delayed_work` at ~30fps
- **MMAP buffer mapping** — userspace access to kernel-allocated buffers
- **Buffer lifecycle** — REQBUFS → QBUF → STREAMON → DQBUF cycle

## Carries Forward from Part 2

- Hardware register access (`ioread32` / `iowrite32`)
- V4L2 device / video_device registration
- Format negotiation ioctls

## NOT Yet Covered (see Part 4)

- Hardware descriptor ring for DMA-style buffer delivery
- Interrupt-driven frame completion

## Build & Test

```bash
# Build hardware platform and kernel module
make -C ../hw
make

# Build test program
gcc -Wall -o test_capture test_capture.c

# Load and test
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_vb2.ko
sudo ./test_capture
```
