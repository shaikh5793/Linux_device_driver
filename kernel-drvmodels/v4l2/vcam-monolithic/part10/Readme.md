# Part 10: Full V4L2 Pipeline (Capstone)

Ties together the entire VCAM driver series into a complete
capture → export → import → output pipeline demonstrating
end-to-end zero-copy video frame flow.

**Source**: `test_pipeline.c` (userspace only — no new kernel module)

## Pipeline Flow

```
vcam_expbuf (Part 7)   →  VIDIOC_EXPBUF  →  dma-buf fd
                                              ├── buf_reader (Part 8) reads frame
                                              └── vout_dmabuf (Part 9) displays frame
```

## Concepts Demonstrated

- **End-to-end V4L2 streaming** — REQBUFS → QBUF → STREAMON → DQBUF cycle
- **VIDIOC_EXPBUF** — exporting a captured frame as a dma-buf fd
- **Cross-device buffer sharing** — same physical buffer read by buf_reader and output device
- **CAPTURE → OUTPUT pipeline** — zero-copy frame handoff between devices
- **DMA-buf import via DMABUF memory mode** — output device imports the fd directly

## Prerequisites

Load the modules from Parts 7, 8, and 9:

```bash
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod ../part7/vcam_expbuf.ko      # capture + export
sudo insmod ../part8/buf_reader.ko        # dma-buf importer
sudo insmod ../part9/vout_dmabuf.ko       # output device
```

## Build & Run

```bash
make
sudo ./test_pipeline
```

## What to Observe

1. Capture device produces a frame with a counter and pixel pattern
2. EXPBUF exports the buffer — same physical pages, no copy
3. buf_reader reads the frame via `dma_buf_vmap()` (check dmesg)
4. Output device imports the dma-buf fd and "displays" the frame
5. Frame counter and pixel values match across all three consumers
