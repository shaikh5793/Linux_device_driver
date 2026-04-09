// SPDX-License-Identifier: GPL-2.0
/*
 * VCAM-2000 Virtual Image Sensor Controller — Hardware Platform
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * This module creates a virtual camera sensor controller that driver
 * modules interact with through a register file and software interrupts,
 * exactly as they would with real hardware.
 *
 * Architecture:
 *   vcam_hw_platform.ko   ← this module (virtual hardware)
 *   vcam_*.ko             ← driver modules (use ioread32/iowrite32)
 *
 * The frame generation engine runs inside this module.  Driver modules
 * never generate pixel data — they only manage buffers, program
 * registers, and handle interrupts.
 *
 * LOAD ORDER:
 *   sudo insmod hw/vcam_hw_platform.ko
 *   sudo insmod partN/vcam_*.ko
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#include "vcam_hw_interface.h"

#define DRV_NAME "vcam_hw_platform"

/* ====================================================================
 * SECTION 1: Hardware Internal State
 * ==================================================================== */

struct vcam_hw_device {
	/* Register file: VCAM_HW_REG_COUNT × 32-bit registers */
	u32 regs[VCAM_HW_REG_COUNT];

	/* Platform devices for driver binding */
	struct platform_device *pdev;		/* capture driver */
	struct platform_device *pdev_out;	/* output driver (part 6) */

	/* Software IRQ */
	int irq;

	/* Frame generation engine */
	struct delayed_work frame_work;
	bool streaming;

	/* Buffer descriptor ring (kernel VA supplied by driver) */
	struct vcam_hw_desc *ring_va;
	u32 ring_count;
	u32 ring_tail;		/* HW pointer: next descriptor to fill */

	/* Statistics backing (64-bit, exposed as 32-bit regs) */
	u64 total_frames;
	u64 total_bytes;
	u64 total_errors;
	u64 total_dropped;
};

static struct vcam_hw_device *hw_dev;

/* ====================================================================
 * SECTION 2: Register File Initialization
 * ==================================================================== */

static void vcam_hw_init_regs(struct vcam_hw_device *dev)
{
	memset(dev->regs, 0, sizeof(dev->regs));

	/* Read-only identification */
	dev->regs[VCAM_CHIP_ID / 4]  = VCAM_HW_CHIP_ID_VAL;
	dev->regs[VCAM_CHIP_REV / 4] = VCAM_HW_CHIP_REV_VAL;

	/* Sensor ready after power-on */
	dev->regs[VCAM_STATUS / 4] = VCAM_STATUS_READY;

	/* All interrupts masked (disabled) by default */
	dev->regs[VCAM_INT_MASK / 4] = 0xFFFFFFFF;

	/* Default format: 640×480 RGB24 */
	dev->regs[VCAM_FMT_WIDTH / 4]     = VCAM_HW_DEF_WIDTH;
	dev->regs[VCAM_FMT_HEIGHT / 4]    = VCAM_HW_DEF_HEIGHT;
	dev->regs[VCAM_FMT_PIXFMT / 4]    = VCAM_HW_FMT_RGB24;
	dev->regs[VCAM_FMT_STRIDE / 4]    = VCAM_HW_DEF_WIDTH * 3;
	dev->regs[VCAM_FMT_FRAMESIZE / 4] = VCAM_HW_DEF_WIDTH * 3 *
					     VCAM_HW_DEF_HEIGHT;

	/* Default image controls */
	dev->regs[VCAM_BRIGHTNESS / 4] = 128;
	dev->regs[VCAM_HFLIP / 4]     = 0;

	/* Zero frame counter */
	dev->regs[VCAM_FRAME_COUNT / 4] = 0;
}

/* ====================================================================
 * SECTION 3: Interrupt Controller
 *
 * INT_MASK:   1 = disabled, 0 = enabled
 * INT_STATUS: pending bits, write-1-to-clear
 *
 * Interrupt fires when: INT_STATUS & ~INT_MASK != 0
 * ==================================================================== */

static void vcam_hw_fire_interrupt(struct vcam_hw_device *dev, u32 bits)
{
	u32 mask, status;

	dev->regs[VCAM_INT_STATUS / 4] |= bits;

	mask   = dev->regs[VCAM_INT_MASK / 4];
	status = dev->regs[VCAM_INT_STATUS / 4];

	/* Fire only if at least one unmasked bit is pending */
	if (status & ~mask) {
		generic_handle_irq_safe(dev->irq);
		/*
		 * W1C emulation: the ISR writes status bits back to
		 * INT_STATUS expecting write-1-to-clear.  Since iowrite32
		 * to a plain array is just a store (no W1C logic), we
		 * clear the fired bits here after the ISR has run.
		 */
		dev->regs[VCAM_INT_STATUS / 4] &= ~bits;
	}
}

/* ====================================================================
 * SECTION 4: Test Pattern Generator
 *
 * Generates an 8-bar color pattern into the supplied buffer.
 * Reads format and control registers to determine output.
 * ==================================================================== */

static const u8 bars_r[] = { 255, 255, 0,   255, 255, 0,   0,   0   };
static const u8 bars_g[] = { 255, 255, 255, 255, 0,   0,   0,   0   };
static const u8 bars_b[] = { 255, 0,   255, 0,   255, 0,   255, 0   };

static void vcam_hw_fill_pattern(struct vcam_hw_device *dev, void *buf,
				 u32 size)
{
	u32 width  = dev->regs[VCAM_FMT_WIDTH / 4];
	u32 height = dev->regs[VCAM_FMT_HEIGHT / 4];
	u32 stride = dev->regs[VCAM_FMT_STRIDE / 4];
	int bright = (int)dev->regs[VCAM_BRIGHTNESS / 4] - 128;
	int hflip  = dev->regs[VCAM_HFLIP / 4];
	u32 seq    = dev->regs[VCAM_FRAME_COUNT / 4];
	u32 bar_w  = width / 8;
	u32 x, y, src_x, bar;
	int r, g, b;
	u8 *dst;

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

			/* Animate: shift bars slowly over time */
			r = (r + (seq & 0x1f)) & 0xff;

			*dst++ = (u8)clamp(r, 0, 255);
			*dst++ = (u8)clamp(g, 0, 255);
			*dst++ = (u8)clamp(b, 0, 255);
		}
	}

	/* Embed frame counter in first 4 bytes */
	if (size >= 4)
		*(u32 *)buf = seq;
}

/* ====================================================================
 * SECTION 5: Frame Generation Engine
 *
 * Runs as a delayed_work at ~30 fps.  Walks the descriptor ring,
 * generates a frame for each OWN-flagged descriptor, then fires
 * the FRAME_DONE interrupt.
 * ==================================================================== */

static void vcam_hw_frame_work(struct work_struct *work)
{
	struct vcam_hw_device *dev =
		container_of(work, struct vcam_hw_device, frame_work.work);
	struct vcam_hw_desc *desc;
	void *buf_va;
	u32 head, tail, framesize;
	bool did_frame = false;
	ktime_t now;

	if (!dev->streaming || !dev->ring_va)
		goto rearm;

	head = dev->regs[VCAM_BUF_RING_HEAD / 4];
	tail = dev->ring_tail;

	/* Process at most one frame per timer tick */
	if (tail == head)
		goto rearm;	/* ring empty */

	desc = &dev->ring_va[tail];

	if (!(desc->flags & VCAM_DESC_OWN)) {
		/* Driver hasn't submitted this slot yet */
		dev->total_dropped++;
		goto rearm;
	}

	/* Reconstruct buffer VA from descriptor address fields */
	buf_va = (void *)((unsigned long)desc->addr_lo |
			  ((unsigned long)desc->addr_hi << 32));
	if (!buf_va) {
		desc->flags = VCAM_DESC_ERROR;
		dev->total_errors++;
		goto advance;
	}

	framesize = dev->regs[VCAM_FMT_FRAMESIZE / 4];

	/* Generate test pattern into the buffer */
	vcam_hw_fill_pattern(dev, buf_va, desc->size);

	/* Update frame metadata */
	now = ktime_get();
	dev->regs[VCAM_FRAME_COUNT / 4]++;
	dev->regs[VCAM_FRAME_TS_LO / 4] = lower_32_bits(now);
	dev->regs[VCAM_FRAME_TS_HI / 4] = upper_32_bits(now);

	/* Mark descriptor done with sequence number */
	desc->flags = VCAM_DESC_DONE |
		      (dev->regs[VCAM_FRAME_COUNT / 4] & VCAM_DESC_SEQ_MASK);

	/* Update statistics */
	dev->total_frames++;
	dev->total_bytes += framesize;
	dev->regs[VCAM_STATS_FRAMES / 4]  = (u32)dev->total_frames;
	dev->regs[VCAM_STATS_BYTES / 4]   = (u32)dev->total_bytes;
	did_frame = true;

advance:
	/* Advance hardware tail pointer */
	dev->ring_tail = (tail + 1) % dev->ring_count;
	dev->regs[VCAM_BUF_RING_TAIL / 4] = dev->ring_tail;

	/* Fire frame-done interrupt */
	if (did_frame)
		vcam_hw_fire_interrupt(dev, VCAM_INT_FRAME_DONE);

rearm:
	if (dev->streaming)
		schedule_delayed_work(&dev->frame_work,
				      msecs_to_jiffies(VCAM_HW_FRAME_MS));
}

/* ====================================================================
 * SECTION 6: Register Access Hooks
 *
 * Called when the driver writes to certain control registers.
 * ==================================================================== */

static void vcam_hw_handle_ctrl_write(struct vcam_hw_device *dev, u32 val)
{
	/* Reset */
	if (val & VCAM_CTRL_RESET) {
		dev->streaming = false;
		cancel_delayed_work_sync(&dev->frame_work);
		vcam_hw_init_regs(dev);
		dev->ring_tail = 0;
		return;
	}

	/* Stream on */
	if ((val & VCAM_CTRL_ENABLE) && (val & VCAM_CTRL_STREAM_ON)) {
		if (!dev->streaming) {
			dev->streaming = true;
			dev->regs[VCAM_STATUS / 4] |= VCAM_STATUS_STREAMING;
			dev->regs[VCAM_FRAME_COUNT / 4] = 0;
			schedule_delayed_work(&dev->frame_work,
					      msecs_to_jiffies(VCAM_HW_FRAME_MS));
			pr_info(DRV_NAME ": streaming started\n");
		}
		return;
	}

	/* Stream off (ENABLE set but STREAM_ON cleared, or ENABLE cleared) */
	if (dev->streaming) {
		dev->streaming = false;
		cancel_delayed_work_sync(&dev->frame_work);
		dev->regs[VCAM_STATUS / 4] &= ~VCAM_STATUS_STREAMING;
		pr_info(DRV_NAME ": streaming stopped\n");
	}
}

/* ====================================================================
 * SECTION 7: ioread32 / iowrite32 Backing
 *
 * The register file is a plain u32 array.  We expose it through
 * an __iomem pointer so that drivers use ioread32/iowrite32,
 * matching the pattern they'd use with real MMIO BAR mappings.
 *
 * Note: on x86 ioread32/iowrite32 to RAM "just works" because the
 * compiler treats __iomem as a normal pointer after the cast.
 * On ARM the __iomem attribute triggers readl/writel which also
 * work on cacheable memory for this virtual-hardware use case.
 * ==================================================================== */

/* ====================================================================
 * SECTION 8: Exported Platform Functions
 * ==================================================================== */

/*
 * vcam_hw_map_regs -- Return an __iomem pointer to the register file
 *
 * In a real driver this would be the result of pci_iomap() or
 * devm_ioremap().  Here it is a direct cast of our u32 array.
 */
void __iomem *vcam_hw_map_regs(void)
{
	if (!hw_dev)
		return NULL;
	return (void __iomem *)hw_dev->regs;
}
EXPORT_SYMBOL_GPL(vcam_hw_map_regs);

void vcam_hw_unmap_regs(void)
{
	/* Nothing to free — register file lives in hw_dev */
}
EXPORT_SYMBOL_GPL(vcam_hw_unmap_regs);

/*
 * vcam_hw_get_irq -- Return the software IRQ number
 *
 * The driver calls request_irq(vcam_hw_get_irq(), handler, ...).
 */
int vcam_hw_get_irq(void)
{
	if (!hw_dev)
		return -ENODEV;
	return hw_dev->irq;
}
EXPORT_SYMBOL_GPL(vcam_hw_get_irq);

/*
 * vcam_hw_set_buf_ring -- Tell hardware where the descriptor ring is
 *
 * @ring_va: kernel virtual address of struct vcam_hw_desc array
 * @count:   number of descriptors in the ring
 *
 * The platform module walks this ring to find OWN-flagged buffers
 * and fill them with frame data.  Analogous to vnet_hw_set_tx_ring().
 */
int vcam_hw_set_buf_ring(void *ring_va, u32 count)
{
	if (!hw_dev)
		return -ENODEV;
	if (!ring_va || count == 0 || count > VCAM_HW_RING_MAX)
		return -EINVAL;

	hw_dev->ring_va    = ring_va;
	hw_dev->ring_count = count;
	hw_dev->ring_tail  = 0;

	hw_dev->regs[VCAM_BUF_RING_SIZE / 4] = count;
	hw_dev->regs[VCAM_BUF_RING_HEAD / 4] = 0;
	hw_dev->regs[VCAM_BUF_RING_TAIL / 4] = 0;

	pr_info(DRV_NAME ": buffer ring set, %u descriptors\n", count);
	return 0;
}
EXPORT_SYMBOL_GPL(vcam_hw_set_buf_ring);

void vcam_hw_clear_buf_ring(void)
{
	if (!hw_dev)
		return;

	hw_dev->ring_va    = NULL;
	hw_dev->ring_count = 0;
	hw_dev->ring_tail  = 0;

	hw_dev->regs[VCAM_BUF_RING_SIZE / 4] = 0;
	hw_dev->regs[VCAM_BUF_RING_HEAD / 4] = 0;
	hw_dev->regs[VCAM_BUF_RING_TAIL / 4] = 0;
}
EXPORT_SYMBOL_GPL(vcam_hw_clear_buf_ring);

/*
 * vcam_hw_get_pdev -- Return the platform device
 *
 * Useful for dev_err/dev_info logging in driver modules.
 */
struct platform_device *vcam_hw_get_pdev(void)
{
	if (!hw_dev)
		return NULL;
	return hw_dev->pdev;
}
EXPORT_SYMBOL_GPL(vcam_hw_get_pdev);

/* ====================================================================
 * SECTION 9: Register Write Intercept
 *
 * Since drivers use iowrite32() directly to the register array,
 * writes to the CTRL register are picked up on the next frame_work
 * tick.  For immediate response to CTRL writes, we also intercept
 * in the iowrite32 path by having the driver call a notification
 * function after writing CTRL.  However, for simplicity in this
 * virtual hardware we rely on the work function checking the CTRL
 * register each tick.
 *
 * The frame_work function reads CTRL to determine streaming state.
 * ==================================================================== */

/*
 * vcam_hw_notify_ctrl -- Called by driver after writing VCAM_CTRL
 *
 * This allows the hardware to react immediately to streaming
 * start/stop rather than waiting for the next timer tick.
 */
void vcam_hw_notify_ctrl(void)
{
	if (!hw_dev)
		return;
	vcam_hw_handle_ctrl_write(hw_dev, hw_dev->regs[VCAM_CTRL / 4]);
}
EXPORT_SYMBOL_GPL(vcam_hw_notify_ctrl);

/* ====================================================================
 * SECTION 10: Module Init / Exit
 * ==================================================================== */

static int __init vcam_hw_platform_init(void)
{
	int ret;

	hw_dev = kzalloc(sizeof(*hw_dev), GFP_KERNEL);
	if (!hw_dev)
		return -ENOMEM;

	/* Create platform devices for capture and output drivers */
	hw_dev->pdev = platform_device_register_simple(VCAM_HW_DEV_NAME,
						       -1, NULL, 0);
	if (IS_ERR(hw_dev->pdev)) {
		ret = PTR_ERR(hw_dev->pdev);
		goto err_free;
	}

	hw_dev->pdev_out = platform_device_register_simple(VCAM_HW_OUT_DEV_NAME,
							   -1, NULL, 0);
	if (IS_ERR(hw_dev->pdev_out)) {
		ret = PTR_ERR(hw_dev->pdev_out);
		goto err_pdev;
	}

	/* Allocate a software IRQ */
	hw_dev->irq = irq_alloc_desc(0);
	if (hw_dev->irq < 0) {
		ret = hw_dev->irq;
		pr_err(DRV_NAME ": failed to allocate IRQ: %d\n", ret);
		goto err_pdev_out;
	}
	irq_set_chip_and_handler(hw_dev->irq, &dummy_irq_chip,
				 handle_simple_irq);

	/* Initialize register file */
	vcam_hw_init_regs(hw_dev);

	/* Initialize frame generation work */
	INIT_DELAYED_WORK(&hw_dev->frame_work, vcam_hw_frame_work);

	pr_info(DRV_NAME ": VCAM-2000 virtual sensor loaded (IRQ %d)\n",
		hw_dev->irq);
	return 0;

err_pdev_out:
	platform_device_unregister(hw_dev->pdev_out);
err_pdev:
	platform_device_unregister(hw_dev->pdev);
err_free:
	kfree(hw_dev);
	hw_dev = NULL;
	return ret;
}

static void __exit vcam_hw_platform_exit(void)
{
	if (!hw_dev)
		return;

	/* Stop frame generation */
	hw_dev->streaming = false;
	cancel_delayed_work_sync(&hw_dev->frame_work);

	/* Free IRQ descriptor */
	if (hw_dev->irq >= 0)
		irq_free_desc(hw_dev->irq);

	/* Remove platform devices */
	platform_device_unregister(hw_dev->pdev_out);
	platform_device_unregister(hw_dev->pdev);

	kfree(hw_dev);
	hw_dev = NULL;

	pr_info(DRV_NAME ": unloaded\n");
}

module_init(vcam_hw_platform_init);
module_exit(vcam_hw_platform_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("VCAM-2000 Virtual Image Sensor Controller Platform");
MODULE_VERSION("1.0.0");
