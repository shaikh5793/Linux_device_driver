# Part 4: V4L2 Capture Device with Descriptor Ring DMA

Replaces Part 3's timer-based frame delivery with a hardware descriptor ring —
the same DMA programming pattern used by real camera ISPs, network cards, and
storage controllers. Completion is polled via a timer; Part 5 upgrades to
interrupt-driven notification.

**Source**: `vcam_ring.c` | **Module**: `vcam_ring.ko`

## Concepts Introduced (over Part 3)

- **Hardware descriptor ring** — array of `vcam_hw_desc` shared between driver and hardware
- **Producer-consumer model** — driver fills descriptors (HEAD), hardware consumes (TAIL)
- **OWN flag handshake** — `VCAM_DESC_OWN` transfers buffer ownership to hardware
- **Doorbell register** — writing HEAD wakes the DMA engine
- **buf_queue → ring submission** — buffers queued before streaming go to a list, then drain into the ring at start_streaming
- **Polled completion** — delayed work reads TAIL register to detect done frames

## Carries Forward from Part 3

- VB2 buffer management and streaming callbacks
- Format negotiation, V4L2/video_device registration
- Hardware register access

## NOT Yet Covered (see Part 5)

- Interrupt-driven frame completion (`request_irq`)
- ISR top-half / bottom-half split

## Hardware Access Pattern

```
start_streaming:
  ring = kcalloc(...)                    ← allocate descriptor ring
  vcam_hw_set_buf_ring(ring, size)       ← tell hardware where ring lives
  for each queued buffer:
    vcam_submit_buffer(buf)              ← fill descriptor, set OWN, write HEAD

poll_work (every ~16ms):
  hw_tail = ioread32(VCAM_BUF_RING_TAIL) ← check hardware progress
  for tail..hw_tail:
    vb2_buffer_done(buf)                  ← return completed frames to VB2
```

## Build & Test

```bash
make -C ../hw
make
gcc -Wall -o test_capture test_capture.c
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_ring.ko
sudo ./test_capture
```
