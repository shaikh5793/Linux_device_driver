// SPDX-License-Identifier: GPL-2.0
/*
 * VSOC-3000 Virtual SoC Camera Platform — Hardware Simulation
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * This module creates a virtual SoC camera pipeline that driver modules
 * interact with through register files, I2C, and software interrupts,
 * exactly as they would with real hardware.
 *
 * Architecture (virtual hardware blocks):
 *   Camera Sensor (I2C @ 0x10) → CSI-2 Receiver → ISP → DMA Engine
 *
 * Each block has its own register space:
 *   - Sensor:  16 × 16-bit registers on virtual I2C bus
 *   - CSI-2:   16 × 32-bit MMIO registers
 *   - ISP:     16 × 32-bit MMIO registers
 *   - DMA:    144 × 32-bit MMIO registers (with descriptor ring)
 *
 * The frame generation engine runs inside this module.  Driver modules
 * never generate pixel data — they manage buffers, program registers,
 * and handle interrupts.
 *
 * LOAD ORDER:
 *   sudo insmod soc/hw/soc_hw_platform.ko
 *   sudo insmod soc/partN/vsoc_*.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#include "soc_hw_interface.h"

#define DRV_NAME "soc_hw_platform"

/* ====================================================================
 * SECTION 1: Hardware Internal State
 * ==================================================================== */

struct vsoc_hw_device {
	/* --- Sensor block (I2C registers) --- */
	u16 sensor_regs[VSOC_SENSOR_REG_COUNT];
	struct i2c_adapter i2c_adap;
	struct i2c_client *sensor_client;

	/* --- CSI-2 receiver block --- */
	u32 csi2_regs[VSOC_CSI2_REG_COUNT];
	struct platform_device *pdev_csi2;

	/* --- ISP block --- */
	u32 isp_regs[VSOC_ISP_REG_COUNT];
	struct platform_device *pdev_isp;

	/* --- DMA engine block --- */
	u32 dma_regs[VSOC_DMA_REG_COUNT];
	struct platform_device *pdev_bridge;
	int dma_irq;

	/* --- Frame generation engine --- */
	struct delayed_work frame_work;
	bool streaming;

	/* --- DMA descriptor ring --- */
	struct vsoc_hw_desc *ring_va;
	u32 ring_count;
	u32 ring_tail;

	/* --- Statistics --- */
	u64 total_frames;
	u64 total_bytes;
	u64 total_errors;
	u64 total_dropped;
};

static struct vsoc_hw_device *hw_dev;

/* ====================================================================
 * SECTION 2: Register Initialization
 * ==================================================================== */

static void vsoc_hw_init_sensor_regs(struct vsoc_hw_device *dev)
{
	memset(dev->sensor_regs, 0, sizeof(dev->sensor_regs));

	/* Read-only identification */
	dev->sensor_regs[VSOC_SENSOR_CHIP_ID / 2]  = VSOC_SENSOR_CHIP_ID_VAL;
	dev->sensor_regs[VSOC_SENSOR_CHIP_REV / 2]  = VSOC_SENSOR_CHIP_REV_VAL;

	/* Default format: 1920×1080 */
	dev->sensor_regs[VSOC_SENSOR_WIDTH / 2]  = VSOC_SENSOR_DEF_WIDTH;
	dev->sensor_regs[VSOC_SENSOR_HEIGHT / 2] = VSOC_SENSOR_DEF_HEIGHT;
	dev->sensor_regs[VSOC_SENSOR_FMT / 2]   = 0x300F; /* SRGGB10 low 16 */

	/* Default controls */
	dev->sensor_regs[VSOC_SENSOR_EXPOSURE / 2] = VSOC_SENSOR_DEF_EXPOSURE;
	dev->sensor_regs[VSOC_SENSOR_GAIN / 2]     = VSOC_SENSOR_DEF_GAIN;
	dev->sensor_regs[VSOC_SENSOR_DGAIN / 2]    = 64;

	/* Status: ready */
	dev->sensor_regs[VSOC_SENSOR_STATUS / 2] = VSOC_SENSOR_STATUS_READY;
}

static void vsoc_hw_init_csi2_regs(struct vsoc_hw_device *dev)
{
	memset(dev->csi2_regs, 0, sizeof(dev->csi2_regs));
	dev->csi2_regs[VSOC_CSI2_STATUS / 4] = VSOC_CSI2_STATUS_READY;
	dev->csi2_regs[VSOC_CSI2_LANES / 4]  = 2;	/* 2-lane default */
	dev->csi2_regs[VSOC_CSI2_INT_MASK / 4] = 0xFFFFFFFF;
}

static void vsoc_hw_init_isp_regs(struct vsoc_hw_device *dev)
{
	memset(dev->isp_regs, 0, sizeof(dev->isp_regs));
	dev->isp_regs[VSOC_ISP_STATUS / 4]     = VSOC_ISP_STATUS_READY;
	dev->isp_regs[VSOC_ISP_BRIGHTNESS / 4]  = 128;
	dev->isp_regs[VSOC_ISP_CONTRAST / 4]    = 128;
	dev->isp_regs[VSOC_ISP_INT_MASK / 4]   = 0xFFFFFFFF;
}

static void vsoc_hw_init_dma_regs(struct vsoc_hw_device *dev)
{
	memset(dev->dma_regs, 0, sizeof(dev->dma_regs));
	dev->dma_regs[VSOC_DMA_STATUS / 4]   = VSOC_DMA_STATUS_READY;
	dev->dma_regs[VSOC_DMA_INT_MASK / 4] = 0xFFFFFFFF;

	/* Default format: 1920×1080 RGB24 */
	dev->dma_regs[VSOC_DMA_FMT_WIDTH / 4]     = VSOC_SENSOR_DEF_WIDTH;
	dev->dma_regs[VSOC_DMA_FMT_HEIGHT / 4]    = VSOC_SENSOR_DEF_HEIGHT;
	dev->dma_regs[VSOC_DMA_FMT_STRIDE / 4]    = VSOC_SENSOR_DEF_WIDTH * 3;
	dev->dma_regs[VSOC_DMA_FMT_FRAMESIZE / 4] = VSOC_SENSOR_DEF_WIDTH * 3 *
						      VSOC_SENSOR_DEF_HEIGHT;
}

/* ====================================================================
 * SECTION 3: Virtual I2C Adapter
 *
 * Creates a virtual I2C bus so sensor subdev drivers probe exactly
 * like real kernel sensor drivers (imx219, ov5640, etc.).
 *
 * The i2c_algorithm reads/writes from the sensor_regs[] array.
 * ==================================================================== */

static int vsoc_i2c_xfer(struct i2c_adapter *adap,
			  struct i2c_msg *msgs, int num)
{
	struct vsoc_hw_device *dev = i2c_get_adapdata(adap);
	int i;

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		u16 reg_addr;

		if (msg->addr != VSOC_SENSOR_I2C_ADDR)
			return -ENXIO;

		if (msg->flags & I2C_M_RD) {
			/*
			 * Read: previous write set the register address.
			 * For SMBus word reads, the address was in the
			 * command byte of the preceding write msg.
			 */
			if (msg->len >= 2) {
				/* Return 16-bit register value (LE) */
				reg_addr = msg->buf[0];
				if (reg_addr / 2 < VSOC_SENSOR_REG_COUNT) {
					u16 val = dev->sensor_regs[reg_addr / 2];
					msg->buf[0] = val & 0xFF;
					msg->buf[1] = (val >> 8) & 0xFF;
				} else {
					msg->buf[0] = 0;
					msg->buf[1] = 0;
				}
			}
		} else {
			/*
			 * Write: first byte is register address (for SMBus
			 * byte/word protocols, the kbuild adapter handles
			 * the framing).
			 */
			if (msg->len >= 3) {
				/* SMBus word write: addr(1) + data(2) */
				reg_addr = msg->buf[0];
				if (reg_addr / 2 < VSOC_SENSOR_REG_COUNT) {
					u16 val = msg->buf[1] |
						  (msg->buf[2] << 8);
					/* Protect read-only registers */
					if (reg_addr != VSOC_SENSOR_CHIP_ID &&
					    reg_addr != VSOC_SENSOR_CHIP_REV &&
					    reg_addr != VSOC_SENSOR_STATUS)
						dev->sensor_regs[reg_addr / 2] = val;
				}
			} else if (msg->len == 1) {
				/*
				 * Single byte write: sets register pointer
				 * for next read. Store in msg->buf[0] for
				 * the following read msg to pick up.
				 */
				if (i + 1 < num &&
				    (msgs[i + 1].flags & I2C_M_RD)) {
					msgs[i + 1].buf[0] = msg->buf[0];
				}
			}
		}
	}

	return num;
}

static u32 vsoc_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE_DATA |
	       I2C_FUNC_SMBUS_WORD_DATA;
}

static const struct i2c_algorithm vsoc_i2c_algo = {
	.master_xfer   = vsoc_i2c_xfer,
	.functionality = vsoc_i2c_func,
};

/* ====================================================================
 * SECTION 4: DMA Interrupt Controller
 * ==================================================================== */

static void vsoc_hw_fire_dma_irq(struct vsoc_hw_device *dev, u32 bits)
{
	u32 mask, status;

	dev->dma_regs[VSOC_DMA_INT_STATUS / 4] |= bits;

	mask   = dev->dma_regs[VSOC_DMA_INT_MASK / 4];
	status = dev->dma_regs[VSOC_DMA_INT_STATUS / 4];

	if (status & ~mask) {
		generic_handle_irq_safe(dev->dma_irq);
		dev->dma_regs[VSOC_DMA_INT_STATUS / 4] &= ~bits;
	}
}

/* ====================================================================
 * SECTION 5: Test Pattern Generator
 * ==================================================================== */

static const u8 bars_r[] = { 255, 255, 0,   255, 255, 0,   0,   0   };
static const u8 bars_g[] = { 255, 255, 255, 255, 0,   0,   0,   0   };
static const u8 bars_b[] = { 255, 0,   255, 0,   255, 0,   255, 0   };

static void vsoc_hw_fill_pattern(struct vsoc_hw_device *dev, void *buf,
				  u32 size)
{
	u32 width  = dev->dma_regs[VSOC_DMA_FMT_WIDTH / 4];
	u32 height = dev->dma_regs[VSOC_DMA_FMT_HEIGHT / 4];
	u32 stride = dev->dma_regs[VSOC_DMA_FMT_STRIDE / 4];
	int bright = (int)dev->isp_regs[VSOC_ISP_BRIGHTNESS / 4] - 128;
	int hflip  = dev->sensor_regs[VSOC_SENSOR_HFLIP / 2];
	u32 seq    = dev->dma_regs[VSOC_DMA_FRAME_COUNT / 4];
	u32 bar_w  = width / 8;
	u32 x, y, src_x, bar;
	int r, g, b;
	u8 *dst;

	if (!width || !height || !stride)
		return;
	if (stride * height > size)
		return;

	for (y = 0; y < height; y++) {
		dst = (u8 *)buf + y * stride;
		for (x = 0; x < width; x++) {
			src_x = hflip ? (width - 1 - x) : x;
			bar = bar_w ? (src_x / bar_w) : 0;
			if (bar > 7)
				bar = 7;

			r = bars_r[bar] + bright;
			g = bars_g[bar] + bright;
			b = bars_b[bar] + bright;

			/* Animate */
			r = (r + (seq & 0x1f)) & 0xff;

			*dst++ = (u8)clamp(r, 0, 255);
			*dst++ = (u8)clamp(g, 0, 255);
			*dst++ = (u8)clamp(b, 0, 255);
		}
	}

	/* Embed frame counter */
	if (size >= 4)
		*(u32 *)buf = seq;
}

/* ====================================================================
 * SECTION 6: Frame Generation Engine
 *
 * Runs as delayed_work at ~30 fps.  Simulates the full pipeline:
 * sensor → CSI-2 → ISP → DMA → memory.
 * ==================================================================== */

static void vsoc_hw_frame_work(struct work_struct *work)
{
	struct vsoc_hw_device *dev =
		container_of(work, struct vsoc_hw_device, frame_work.work);
	struct vsoc_hw_desc *desc;
	void *buf_va;
	u32 head, tail, framesize;
	bool did_frame = false;
	ktime_t now;

	if (!dev->streaming || !dev->ring_va)
		goto rearm;

	head = dev->dma_regs[VSOC_DMA_BUF_RING_HEAD / 4];
	tail = dev->ring_tail;

	if (tail == head)
		goto rearm;

	desc = &dev->ring_va[tail % dev->ring_count];

	if (!(desc->flags & VSOC_DESC_OWN)) {
		dev->total_dropped++;
		goto rearm;
	}

	buf_va = (void *)((unsigned long)desc->addr_lo |
			  ((unsigned long)desc->addr_hi << 32));
	if (!buf_va) {
		desc->flags = VSOC_DESC_ERROR;
		dev->total_errors++;
		goto advance;
	}

	framesize = dev->dma_regs[VSOC_DMA_FMT_FRAMESIZE / 4];

	/* Generate test pattern */
	vsoc_hw_fill_pattern(dev, buf_va, desc->size);

	/* Update frame metadata */
	now = ktime_get();
	dev->dma_regs[VSOC_DMA_FRAME_COUNT / 4]++;
	dev->dma_regs[VSOC_DMA_FRAME_TS_LO / 4] = lower_32_bits(now);
	dev->dma_regs[VSOC_DMA_FRAME_TS_HI / 4] = upper_32_bits(now);

	/* Mark descriptor done */
	desc->flags = VSOC_DESC_DONE |
		      (dev->dma_regs[VSOC_DMA_FRAME_COUNT / 4] &
		       VSOC_DESC_SEQ_MASK);

	/* Statistics */
	dev->total_frames++;
	dev->total_bytes += framesize;
	dev->dma_regs[VSOC_DMA_STATS_FRAMES / 4]  = (u32)dev->total_frames;
	dev->dma_regs[VSOC_DMA_STATS_BYTES / 4]   = (u32)dev->total_bytes;
	did_frame = true;

advance:
	dev->ring_tail = tail + 1;
	dev->dma_regs[VSOC_DMA_BUF_RING_TAIL / 4] = dev->ring_tail;

	if (did_frame)
		vsoc_hw_fire_dma_irq(dev, VSOC_DMA_INT_FRAME_DONE);

rearm:
	if (dev->streaming)
		schedule_delayed_work(&dev->frame_work,
				      msecs_to_jiffies(VSOC_HW_FRAME_MS));
}

/* ====================================================================
 * SECTION 7: Exported Functions — I2C Adapter
 * ==================================================================== */

struct i2c_adapter *vsoc_hw_get_i2c_adapter(void)
{
	if (!hw_dev)
		return NULL;
	return &hw_dev->i2c_adap;
}
EXPORT_SYMBOL_GPL(vsoc_hw_get_i2c_adapter);

/* ====================================================================
 * SECTION 8: Exported Functions — MMIO Register Maps
 * ==================================================================== */

void __iomem *vsoc_hw_map_csi2_regs(void)
{
	if (!hw_dev)
		return NULL;
	return (void __iomem *)hw_dev->csi2_regs;
}
EXPORT_SYMBOL_GPL(vsoc_hw_map_csi2_regs);

void vsoc_hw_unmap_csi2_regs(void)
{
}
EXPORT_SYMBOL_GPL(vsoc_hw_unmap_csi2_regs);

void __iomem *vsoc_hw_map_isp_regs(void)
{
	if (!hw_dev)
		return NULL;
	return (void __iomem *)hw_dev->isp_regs;
}
EXPORT_SYMBOL_GPL(vsoc_hw_map_isp_regs);

void vsoc_hw_unmap_isp_regs(void)
{
}
EXPORT_SYMBOL_GPL(vsoc_hw_unmap_isp_regs);

void __iomem *vsoc_hw_map_dma_regs(void)
{
	if (!hw_dev)
		return NULL;
	return (void __iomem *)hw_dev->dma_regs;
}
EXPORT_SYMBOL_GPL(vsoc_hw_map_dma_regs);

void vsoc_hw_unmap_dma_regs(void)
{
}
EXPORT_SYMBOL_GPL(vsoc_hw_unmap_dma_regs);

/* ====================================================================
 * SECTION 9: Exported Functions — DMA IRQ and Buffer Ring
 * ==================================================================== */

int vsoc_hw_get_dma_irq(void)
{
	if (!hw_dev)
		return -ENODEV;
	return hw_dev->dma_irq;
}
EXPORT_SYMBOL_GPL(vsoc_hw_get_dma_irq);

int vsoc_hw_set_buf_ring(void *ring_va, u32 count)
{
	if (!hw_dev)
		return -ENODEV;
	if (!ring_va || count == 0 || count > VSOC_HW_RING_MAX)
		return -EINVAL;

	hw_dev->ring_va    = ring_va;
	hw_dev->ring_count = count;
	hw_dev->ring_tail  = 0;

	hw_dev->dma_regs[VSOC_DMA_BUF_RING_SIZE / 4] = count;
	hw_dev->dma_regs[VSOC_DMA_BUF_RING_HEAD / 4] = 0;
	hw_dev->dma_regs[VSOC_DMA_BUF_RING_TAIL / 4] = 0;

	pr_info(DRV_NAME ": DMA buffer ring set, %u descriptors\n", count);
	return 0;
}
EXPORT_SYMBOL_GPL(vsoc_hw_set_buf_ring);

void vsoc_hw_clear_buf_ring(void)
{
	if (!hw_dev)
		return;

	hw_dev->ring_va    = NULL;
	hw_dev->ring_count = 0;
	hw_dev->ring_tail  = 0;

	hw_dev->dma_regs[VSOC_DMA_BUF_RING_SIZE / 4] = 0;
	hw_dev->dma_regs[VSOC_DMA_BUF_RING_HEAD / 4] = 0;
	hw_dev->dma_regs[VSOC_DMA_BUF_RING_TAIL / 4] = 0;
}
EXPORT_SYMBOL_GPL(vsoc_hw_clear_buf_ring);

/* ====================================================================
 * SECTION 10: Exported Functions — Streaming Notification
 * ==================================================================== */

void vsoc_hw_notify_stream(int on)
{
	if (!hw_dev)
		return;

	if (on && !hw_dev->streaming) {
		hw_dev->streaming = true;
		hw_dev->dma_regs[VSOC_DMA_STATUS / 4] |=
			VSOC_DMA_STATUS_STREAMING;
		hw_dev->dma_regs[VSOC_DMA_FRAME_COUNT / 4] = 0;
		schedule_delayed_work(&hw_dev->frame_work,
				      msecs_to_jiffies(VSOC_HW_FRAME_MS));
		pr_info(DRV_NAME ": pipeline streaming started\n");
	} else if (!on && hw_dev->streaming) {
		hw_dev->streaming = false;
		cancel_delayed_work_sync(&hw_dev->frame_work);
		hw_dev->dma_regs[VSOC_DMA_STATUS / 4] &=
			~VSOC_DMA_STATUS_STREAMING;
		pr_info(DRV_NAME ": pipeline streaming stopped\n");
	}
}
EXPORT_SYMBOL_GPL(vsoc_hw_notify_stream);

/* ====================================================================
 * SECTION 11: Exported Functions — Platform Devices
 * ==================================================================== */

struct platform_device *vsoc_hw_get_bridge_pdev(void)
{
	return hw_dev ? hw_dev->pdev_bridge : NULL;
}
EXPORT_SYMBOL_GPL(vsoc_hw_get_bridge_pdev);

struct platform_device *vsoc_hw_get_csi2_pdev(void)
{
	return hw_dev ? hw_dev->pdev_csi2 : NULL;
}
EXPORT_SYMBOL_GPL(vsoc_hw_get_csi2_pdev);

struct platform_device *vsoc_hw_get_isp_pdev(void)
{
	return hw_dev ? hw_dev->pdev_isp : NULL;
}
EXPORT_SYMBOL_GPL(vsoc_hw_get_isp_pdev);

/* ====================================================================
 * SECTION 12: Module Init / Exit
 * ==================================================================== */

static const struct i2c_board_info vsoc_sensor_board_info = {
	I2C_BOARD_INFO("vsoc_sensor", VSOC_SENSOR_I2C_ADDR),
};

static int __init vsoc_hw_platform_init(void)
{
	int ret;

	hw_dev = kzalloc(sizeof(*hw_dev), GFP_KERNEL);
	if (!hw_dev)
		return -ENOMEM;

	/* --- Initialize all register files --- */
	vsoc_hw_init_sensor_regs(hw_dev);
	vsoc_hw_init_csi2_regs(hw_dev);
	vsoc_hw_init_isp_regs(hw_dev);
	vsoc_hw_init_dma_regs(hw_dev);

	/* --- Set up virtual I2C adapter --- */
	hw_dev->i2c_adap.owner = THIS_MODULE;
	hw_dev->i2c_adap.algo  = &vsoc_i2c_algo;
	hw_dev->i2c_adap.nr    = -1;	/* auto-assign bus number */
	strscpy(hw_dev->i2c_adap.name, "vsoc-i2c",
		sizeof(hw_dev->i2c_adap.name));
	i2c_set_adapdata(&hw_dev->i2c_adap, hw_dev);

	ret = i2c_add_adapter(&hw_dev->i2c_adap);
	if (ret) {
		pr_err(DRV_NAME ": failed to add I2C adapter: %d\n", ret);
		goto err_free;
	}

	/* Instantiate the virtual sensor on the I2C bus */
	hw_dev->sensor_client = i2c_new_client_device(&hw_dev->i2c_adap,
						       &vsoc_sensor_board_info);
	if (IS_ERR(hw_dev->sensor_client)) {
		ret = PTR_ERR(hw_dev->sensor_client);
		pr_err(DRV_NAME ": failed to create I2C sensor: %d\n", ret);
		goto err_i2c;
	}

	/* --- Create platform devices for SoC blocks --- */
	hw_dev->pdev_bridge = platform_device_register_simple(
				VSOC_BRIDGE_DEV_NAME, -1, NULL, 0);
	if (IS_ERR(hw_dev->pdev_bridge)) {
		ret = PTR_ERR(hw_dev->pdev_bridge);
		goto err_sensor;
	}

	hw_dev->pdev_csi2 = platform_device_register_simple(
				VSOC_CSI2_DEV_NAME, -1, NULL, 0);
	if (IS_ERR(hw_dev->pdev_csi2)) {
		ret = PTR_ERR(hw_dev->pdev_csi2);
		goto err_bridge;
	}

	hw_dev->pdev_isp = platform_device_register_simple(
				VSOC_ISP_DEV_NAME, -1, NULL, 0);
	if (IS_ERR(hw_dev->pdev_isp)) {
		ret = PTR_ERR(hw_dev->pdev_isp);
		goto err_csi2;
	}

	/* --- Allocate DMA software IRQ --- */
	hw_dev->dma_irq = irq_alloc_desc(0);
	if (hw_dev->dma_irq < 0) {
		ret = hw_dev->dma_irq;
		pr_err(DRV_NAME ": failed to allocate DMA IRQ: %d\n", ret);
		goto err_isp;
	}
	irq_set_chip_and_handler(hw_dev->dma_irq, &dummy_irq_chip,
				 handle_simple_irq);

	/* --- Initialize frame generation engine --- */
	INIT_DELAYED_WORK(&hw_dev->frame_work, vsoc_hw_frame_work);

	pr_info(DRV_NAME ": VSOC-3000 virtual SoC loaded\n");
	pr_info(DRV_NAME ":   I2C bus %d, sensor @ 0x%02x\n",
		hw_dev->i2c_adap.nr, VSOC_SENSOR_I2C_ADDR);
	pr_info(DRV_NAME ":   Platform: %s, %s, %s\n",
		VSOC_BRIDGE_DEV_NAME, VSOC_CSI2_DEV_NAME, VSOC_ISP_DEV_NAME);
	pr_info(DRV_NAME ":   DMA IRQ %d\n", hw_dev->dma_irq);
	return 0;

err_isp:
	platform_device_unregister(hw_dev->pdev_isp);
err_csi2:
	platform_device_unregister(hw_dev->pdev_csi2);
err_bridge:
	platform_device_unregister(hw_dev->pdev_bridge);
err_sensor:
	i2c_unregister_device(hw_dev->sensor_client);
err_i2c:
	i2c_del_adapter(&hw_dev->i2c_adap);
err_free:
	kfree(hw_dev);
	hw_dev = NULL;
	return ret;
}

static void __exit vsoc_hw_platform_exit(void)
{
	if (!hw_dev)
		return;

	/* Stop frame generation */
	hw_dev->streaming = false;
	cancel_delayed_work_sync(&hw_dev->frame_work);

	/* Free DMA IRQ */
	if (hw_dev->dma_irq >= 0)
		irq_free_desc(hw_dev->dma_irq);

	/* Remove platform devices */
	platform_device_unregister(hw_dev->pdev_isp);
	platform_device_unregister(hw_dev->pdev_csi2);
	platform_device_unregister(hw_dev->pdev_bridge);

	/* Remove I2C sensor and adapter */
	i2c_unregister_device(hw_dev->sensor_client);
	i2c_del_adapter(&hw_dev->i2c_adap);

	kfree(hw_dev);
	hw_dev = NULL;

	pr_info(DRV_NAME ": unloaded\n");
}

module_init(vsoc_hw_platform_init);
module_exit(vsoc_hw_platform_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VSOC-3000 Virtual SoC Camera Platform");
MODULE_VERSION("1.0.0");
