# Part 8: dma-buf Consumer (buf_reader Importer)

Introduces the import side of dma-buf sharing. The `buf_reader` misc device
receives a dma-buf file descriptor via ioctl, maps the buffer using
`dma_buf_vmap()`, and reads the frame contents — demonstrating cross-device
zero-copy buffer consumption.

**Source**: `buf_reader.c` | **Module**: `buf_reader.ko`

## Concepts Introduced (over Part 7)

- **dma-buf import** — `dma_buf_get()` to obtain a dma_buf from an fd
- **CPU access protocol** — `dma_buf_begin_cpu_access()` / `end_cpu_access()`
- **Kernel mapping** — `dma_buf_vmap()` for kernel virtual address access
- **iosys_map abstraction** — unified mapping for system and I/O memory
- **Misc device** — simple character device for ioctl-only interface

## Carries Forward from Part 7

- VIDIOC_EXPBUF export (the producer side)
- Interrupt-driven descriptor ring streaming
- VB2 buffer management

## NOT Yet Covered (see Part 9)

- V4L2 OUTPUT device type
- dma-buf import via V4L2 DMABUF memory mode

## Build & Test

```bash
# Build (no hw dependency — standalone module)
make

# Load (Part 7 must be loaded first to produce dma-buf fds)
sudo insmod buf_reader.ko

# Test: use test_expbuf from Part 7 which passes fds to buf_reader
sudo ../part7/test_expbuf
```
