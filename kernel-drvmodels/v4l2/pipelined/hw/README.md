# HW: VSOC-3000 Virtual SoC Camera Platform

Copyright (c) 2024 TECH VEDA. Author: Raghu Bharadwaj.

## Purpose

This module creates the **virtual hardware** that all SoC driver modules (parts 1-12) interact with. It simulates a multi-block camera SoC containing a sensor on an I2C bus, a CSI-2 receiver, an ISP, and a DMA engine -- each with its own register space. The frame generation engine runs inside this module at ~30 fps. Driver modules never generate pixel data; they program registers, manage buffers, and handle interrupts, exactly as they would with real silicon.

## Architecture

```
Camera Sensor (I2C @ 0x10) --> CSI-2 Receiver --> ISP --> DMA Engine
   16x16-bit regs              16x32-bit MMIO    16x32-bit MMIO   144x32-bit MMIO
   (virtual I2C bus)                                               (descriptor ring)
```

## Key Components

### Virtual I2C Adapter
- Creates a real `struct i2c_adapter` with `i2c_add_adapter()`
- The `i2c_algorithm` reads/writes from the internal `sensor_regs[]` array
- Instantiates an `i2c_client` at address `0x10` so sensor subdev drivers probe via standard I2C mechanisms (just like `imx219`, `ov5640`, etc.)

### Platform Devices
- Registers three `platform_device` instances: `vsoc_bridge`, `vsoc_csi2`, `vsoc_isp`
- Driver modules in each part are platform drivers that match these names

### Register Maps
- **Sensor:** 16 x 16-bit registers (chip ID, format, controls, status)
- **CSI-2:** 16 x 32-bit registers (ctrl, status, format, lanes, interrupts)
- **ISP:** 16 x 32-bit registers (ctrl, status, in/out format, brightness, contrast)
- **DMA:** 144 x 32-bit registers (ctrl, status, format, buffer ring, frame count, statistics)

### DMA Descriptor Ring
- Uses `struct vsoc_hw_desc` (16 bytes: addr_lo, addr_hi, size, flags)
- Ring of up to 16 descriptors, managed by head/tail pointers
- Flags: `VSOC_DESC_OWN` (hardware-owned), `VSOC_DESC_DONE`, `VSOC_DESC_ERROR`

### Frame Generation Engine
- Runs as `delayed_work` at ~30 fps (`VSOC_HW_FRAME_MS = 33`)
- Generates color bar test patterns with brightness/flip modifiers
- Fires a software IRQ (`generic_handle_irq_safe`) on frame completion

### DMA IRQ
- Allocates a software IRQ descriptor via `irq_alloc_desc()`
- Bridge drivers request this IRQ to handle frame-done events

## Exported Functions

| Function | Description |
|----------|-------------|
| `vsoc_hw_get_i2c_adapter()` | Returns the virtual I2C adapter |
| `vsoc_hw_map_csi2_regs()` | Returns MMIO pointer to CSI-2 register block |
| `vsoc_hw_map_isp_regs()` | Returns MMIO pointer to ISP register block |
| `vsoc_hw_map_dma_regs()` | Returns MMIO pointer to DMA register block |
| `vsoc_hw_unmap_csi2_regs()` | Release CSI-2 register mapping |
| `vsoc_hw_unmap_isp_regs()` | Release ISP register mapping |
| `vsoc_hw_unmap_dma_regs()` | Release DMA register mapping |
| `vsoc_hw_get_dma_irq()` | Returns the DMA software IRQ number |
| `vsoc_hw_set_buf_ring()` | Configure the DMA descriptor ring |
| `vsoc_hw_clear_buf_ring()` | Clear the DMA descriptor ring |
| `vsoc_hw_notify_stream()` | Start/stop the frame generation engine |
| `vsoc_hw_get_bridge_pdev()` | Returns the bridge platform_device |
| `vsoc_hw_get_csi2_pdev()` | Returns the CSI-2 platform_device |
| `vsoc_hw_get_isp_pdev()` | Returns the ISP platform_device |

## Files

| File | Description |
|------|-------------|
| soc_hw_platform.c | Virtual hardware module: registers, I2C adapter, frame engine, exports |
| soc_hw_interface.h | "Datasheet in code": register maps, bit fields, descriptor format, API prototypes |
| Makefile | Builds soc_hw_platform.ko |

## Build & Run

```bash
# Build
make

# Load (must be loaded FIRST, before any partN module)
sudo insmod soc_hw_platform.ko

# Verify
dmesg | tail -5

# Unload (must be unloaded LAST, after all partN modules)
sudo rmmod soc_hw_platform
```

## Expected Output

```
soc_hw_platform: VSOC-3000 virtual SoC loaded
soc_hw_platform:   I2C bus 7, sensor @ 0x10
soc_hw_platform:   Platform: vsoc_bridge, vsoc_csi2, vsoc_isp
soc_hw_platform:   DMA IRQ 42
```

## Load Order

**Always load `soc_hw_platform.ko` first** -- it creates the I2C bus and platform devices that driver modules bind to. Always unload it last.

## Presentation Reference

Slides 1-5 of the V4L2 Subsystem Training deck (SoC Camera Architecture, Hardware Blocks).
