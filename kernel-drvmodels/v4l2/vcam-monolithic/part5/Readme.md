# Part 5: V4L2 Capture Device with Interrupt-Driven Frame Completion

Replaces Part 4's polled completion with interrupt-driven notification — the
standard pattern for production camera and DMA drivers. The ISR acknowledges
the interrupt and schedules a workqueue bottom half for heavy processing.

**Source**: `vcam_irq.c` | **Module**: `vcam_irq.ko`

## Concepts Introduced (over Part 4)

- **request_irq()** — register an interrupt handler for `VCAM_INT_FRAME_DONE`
- **ISR top-half** — runs in hardirq context; read status, acknowledge, schedule work
- **IRQ bottom-half via workqueue** — deferred processing in process context
- **Why the split?** — `vb2_buffer_done()` may sleep; cannot be called in hardirq
- **Interrupt masking** — `VCAM_INT_MASK` register controls which interrupts fire
- **W1C (write-1-to-clear)** — acknowledge pattern for `VCAM_INT_STATUS`

## Carries Forward from Part 4

- Hardware descriptor ring with OWN flag handshake
- VB2 buffer management and streaming
- Format negotiation, V4L2/video_device registration

## NOT Yet Covered (see Part 6)

- V4L2 controls framework (brightness, hflip)

## ISR / Bottom-Half Flow

```
Hardware fires VCAM_INT_FRAME_DONE
  │
  ▼
vcam_isr() [hardirq context — must be fast]
  ├── ioread32(INT_STATUS)     ← identify interrupt source
  ├── iowrite32(INT_STATUS)    ← acknowledge (W1C)
  └── schedule_work(irq_work)  ← defer heavy processing
        │
        ▼
vcam_irq_work() [process context — can sleep]
  ├── ioread32(RING_TAIL)      ← how far has hardware progressed?
  ├── for each completed desc:
  │     ├── timestamp + sequence
  │     └── vb2_buffer_done()  ← return frame to userspace
  └── update driver tail
```

## Build & Test

```bash
make -C ../hw
make
gcc -Wall -o test_capture test_capture.c
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_irq.ko
sudo ./test_capture
```
