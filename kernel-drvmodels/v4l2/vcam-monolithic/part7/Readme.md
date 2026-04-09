# Part 7: V4L2 Capture Device with dma-buf Export (EXPBUF)

Enables exporting captured V4L2 buffers as dma-buf file descriptors via
`VIDIOC_EXPBUF`. This allows other kernel modules or devices to access
the same physical buffer pages — zero-copy buffer sharing.

**Source**: `vcam_expbuf.c` | **Module**: `vcam_expbuf.ko`

## Concepts Introduced (over Part 6)

- **VIDIOC_EXPBUF ioctl** — export a VB2 buffer as a dma-buf file descriptor
- **VB2_DMABUF io_mode** — enables dma-buf operations on the VB2 queue
- **Zero-copy sharing** — same physical pages accessed by multiple drivers
- **dma-buf fd** — file descriptor that can be passed to any importer

## Carries Forward from Part 6

- V4L2 controls (brightness, hflip)
- Interrupt-driven descriptor ring streaming
- VB2 buffer management, format negotiation

## NOT Yet Covered (see Part 8)

- dma-buf importer module (buf_reader)

## Build & Test

```bash
make -C ../hw
make
gcc -Wall -o test_expbuf test_expbuf.c
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_expbuf.ko
sudo ./test_expbuf   # will fail at buf_reader step until Part 8 is loaded
```
