/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VCAM-2000 Virtual Image Sensor Controller — Hardware Interface
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * This header defines the register map, bit fields, descriptor format,
 * and platform module interface for the VCAM-2000 virtual camera
 * controller.  It is the "datasheet in code" — every driver module
 * includes this file.
 *
 * See hw/VCAM-2000-Datasheet.md for the full hardware specification.
 */
#ifndef VCAM_HW_INTERFACE_H
#define VCAM_HW_INTERFACE_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>

/* ---- Section 1: Core Control Registers (Datasheet §2.1) ---- */

#define VCAM_CTRL		0x000
#define VCAM_STATUS		0x004
#define VCAM_INT_MASK		0x008
#define VCAM_INT_STATUS		0x00C
#define VCAM_CHIP_ID		0x010
#define VCAM_CHIP_REV		0x014

/* ---- Section 2: Format Configuration Registers (§2.2) ---- */

#define VCAM_FMT_WIDTH		0x020
#define VCAM_FMT_HEIGHT		0x024
#define VCAM_FMT_PIXFMT		0x028
#define VCAM_FMT_STRIDE		0x02C
#define VCAM_FMT_FRAMESIZE	0x030

/* ---- Section 3: Image Control Registers (§2.3) ---- */

#define VCAM_BRIGHTNESS		0x040
#define VCAM_HFLIP		0x044

/* ---- Section 4: DMA / Buffer Ring Registers (§2.4) ---- */

#define VCAM_BUF_RING_ADDR	0x100
#define VCAM_BUF_RING_SIZE	0x104
#define VCAM_BUF_RING_HEAD	0x108
#define VCAM_BUF_RING_TAIL	0x10C

/* ---- Section 5: Frame Status Registers (§2.5) ---- */

#define VCAM_FRAME_COUNT	0x0C0
#define VCAM_FRAME_TS_LO	0x0C4
#define VCAM_FRAME_TS_HI	0x0C8

/* ---- Section 6: Statistics Registers (§2.6) ---- */

#define VCAM_STATS_FRAMES	0x200
#define VCAM_STATS_BYTES	0x204
#define VCAM_STATS_ERRORS	0x208
#define VCAM_STATS_DROPPED	0x20C

/* ==================================================================
 * Control Register Bits (0x000)
 * ================================================================== */

#define VCAM_CTRL_ENABLE	BIT(0)	/* Power on sensor */
#define VCAM_CTRL_STREAM_ON	BIT(1)	/* Start frame capture */
#define VCAM_CTRL_RESET		BIT(4)	/* Software reset */
#define VCAM_CTRL_RING_ENABLE	BIT(5)	/* Enable descriptor ring mode */

/* ==================================================================
 * Status Register Bits (0x004)
 * ================================================================== */

#define VCAM_STATUS_READY	BIT(0)	/* Sensor initialized and ready */
#define VCAM_STATUS_STREAMING	BIT(1)	/* Currently streaming frames */

/* ==================================================================
 * Interrupt Bits (0x008, 0x00C)
 * Mask convention: 1 = disabled, 0 = enabled  (same as VNET)
 * ================================================================== */

#define VCAM_INT_FRAME_DONE	BIT(0)	/* Frame capture + DMA complete */
#define VCAM_INT_ERROR		BIT(1)	/* Error (overflow, fault) */
#define VCAM_INT_OVERFLOW	BIT(2)	/* Ring buffer overflow */

#define VCAM_INT_ALL		(VCAM_INT_FRAME_DONE | VCAM_INT_ERROR | \
				 VCAM_INT_OVERFLOW)

/* ==================================================================
 * Buffer Descriptor Format  (16 bytes, §4)
 * ================================================================== */

struct vcam_hw_desc {
	u32 addr_lo;	/* Buffer address (low 32 bits)  */
	u32 addr_hi;	/* Buffer address (high 32 bits) */
	u32 size;	/* Buffer size in bytes          */
	u32 flags;	/* Ownership and status flags    */
};

/* Descriptor flag bits */
#define VCAM_DESC_OWN		BIT(31)	/* 1 = owned by hardware */
#define VCAM_DESC_DONE		BIT(30)	/* Frame written to buffer */
#define VCAM_DESC_ERROR		BIT(29)	/* Error on this frame */
#define VCAM_DESC_SEQ_MASK	0x0000FFFF /* Sequence number low 16 */

/* ==================================================================
 * Hardware Constants (§1)
 * ================================================================== */

#define VCAM_HW_CHIP_ID_VAL	0x00CA2000
#define VCAM_HW_CHIP_REV_VAL	0x01
#define VCAM_HW_REG_COUNT	144	/* Must cover VCAM_STATS_DROPPED (0x20C) + 1 */
#define VCAM_HW_FRAME_MS	33	/* ~30 fps default */

#define VCAM_HW_MIN_WIDTH	160
#define VCAM_HW_MAX_WIDTH	1920
#define VCAM_HW_MIN_HEIGHT	120
#define VCAM_HW_MAX_HEIGHT	1080
#define VCAM_HW_DEF_WIDTH	640
#define VCAM_HW_DEF_HEIGHT	480
#define VCAM_HW_STEP		16

#define VCAM_HW_RING_MAX	16
#define VCAM_HW_DEV_NAME	"vcam_hw"
#define VCAM_HW_OUT_DEV_NAME	"vcam_hw_out"

/* Hardware pixel format codes (V4L2 fourcc values) */
#define VCAM_HW_FMT_RGB24	0x33424752 /* v4l2_fourcc('R','G','B','3') */

/* ==================================================================
 * Platform Module Interface  (§9)
 *
 * Exported by vcam_hw_platform.ko for use by driver modules.
 *
 * Typical probe():
 *   regs = vcam_hw_map_regs();
 *   irq  = vcam_hw_get_irq();
 *   chip = ioread32(regs + VCAM_CHIP_ID);
 *
 * Typical start_streaming():
 *   vcam_hw_set_buf_ring(ring_va, ring_count);
 *   iowrite32(VCAM_CTRL_ENABLE | VCAM_CTRL_STREAM_ON |
 *             VCAM_CTRL_RING_ENABLE, regs + VCAM_CTRL);
 *
 * Typical stop_streaming():
 *   iowrite32(0, regs + VCAM_CTRL);
 *   vcam_hw_clear_buf_ring();
 * ================================================================== */

extern void __iomem *vcam_hw_map_regs(void);
extern void          vcam_hw_unmap_regs(void);
extern int           vcam_hw_get_irq(void);
extern int           vcam_hw_set_buf_ring(void *ring_va, u32 count);
extern void          vcam_hw_clear_buf_ring(void);
extern struct platform_device *vcam_hw_get_pdev(void);
extern void          vcam_hw_notify_ctrl(void);

#endif /* VCAM_HW_INTERFACE_H */
