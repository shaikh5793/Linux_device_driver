# Part 6: V4L2 Capture Device with Controls

Adds the V4L2 controls framework to the interrupt-driven capture device.
Controls expose tunable parameters (brightness, horizontal flip) to userspace
via `VIDIOC_QUERYCTRL`, `VIDIOC_G_CTRL`, and `VIDIOC_S_CTRL`.

**Source**: `vcam_ctrl.c` | **Module**: `vcam_ctrl.ko`

## Concepts Introduced (over Part 5)

- **V4L2 Controls Framework** — `v4l2_ctrl_handler` with `v4l2_ctrl_new_std()`
- **s_ctrl callback** — write control values to hardware registers
- **Hardware image-processing registers** — `VCAM_BRIGHTNESS`, `VCAM_HFLIP`
- **Framework automation** — `QUERYCTRL`, `G_CTRL`, `S_CTRL` handled by V4L2 core
- **Control handler wiring** — `v4l2_dev.ctrl_handler` and `vdev->ctrl_handler`

## Carries Forward from Part 5

- Interrupt-driven frame completion (ISR + workqueue)
- Hardware descriptor ring with OWN flag handshake
- VB2 streaming, format negotiation

## NOT Yet Covered (see Part 7)

- dma-buf export (VIDIOC_EXPBUF)

## Build & Test

```bash
make -C ../hw
make
gcc -Wall -o test_controls test_controls.c
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_ctrl.ko
sudo ./test_controls
```
