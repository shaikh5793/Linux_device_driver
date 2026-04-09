# Part 9: V4L2 Output Device with dma-buf Import

Introduces the V4L2 OUTPUT device type — the reverse of a capture device.
Userspace pushes frame data to the driver (e.g., for display). Buffers
are imported via `V4L2_MEMORY_DMABUF`, enabling zero-copy from a capture
device or DMA heap allocation.

**Source**: `vout_dmabuf.c` | **Module**: `vout_dmabuf.ko`

## Concepts Introduced (over Part 8)

- **V4L2 OUTPUT device type** — `V4L2_BUF_TYPE_VIDEO_OUTPUT`
- **VFL_DIR_TX flag** — marks device as output for ioctl validation
- **dma-buf import** — `V4L2_MEMORY_DMABUF` in REQBUFS/QBUF
- **Reversed data flow** — userspace produces, driver consumes
- **DMA heap allocation** — test program allocates from `/dev/dma_heap/system`

## Carries Forward

- VB2 buffer management, workqueue processing
- Hardware register access (ioread32/iowrite32)

## NOT Yet Covered (see Part 10)

- Full capture → export → import → output pipeline (capstone)

## Build & Test

```bash
make -C ../hw
make
gcc -Wall -o test_import test_import.c
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vout_dmabuf.ko
sudo ./test_import
```
