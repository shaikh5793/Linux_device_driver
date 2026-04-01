/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VSOC-3000 Virtual SoC Camera Platform — Hardware Interface
 *
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 *
 * This header defines register maps, bit fields, descriptor formats,
 * and platform module interface for the VSOC-3000 virtual SoC camera
 * pipeline.  It is the "datasheet in code" — every driver module in
 * the SoC series includes this file.
 *
 * The VSOC-3000 models a multi-component camera pipeline:
 *   Camera Sensor (I2C) → CSI-2 Receiver → ISP → DMA Engine
 *
 * Each block has its own register space and is accessed independently.
 */
#ifndef SOC_HW_INTERFACE_H
#define SOC_HW_INTERFACE_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>

/* ====================================================================
 * Section 1: Sensor Registers (I2C, 16-bit addresses, 16-bit values)
 *
 * Accessed via i2c_smbus_read/write_word_data() at address 0x10.
 * ==================================================================== */

#define VSOC_SENSOR_I2C_ADDR		0x10

/* Identification */
#define VSOC_SENSOR_CHIP_ID		0x00	/* RO: 0x3000 */
#define VSOC_SENSOR_CHIP_REV		0x02	/* RO: 0x01 */

/* Control */
#define VSOC_SENSOR_CTRL		0x04
#define VSOC_SENSOR_CTRL_ENABLE		BIT(0)	/* Power on */
#define VSOC_SENSOR_CTRL_STREAM		BIT(1)	/* Start streaming */

/* Format configuration */
#define VSOC_SENSOR_WIDTH		0x06	/* Active width */
#define VSOC_SENSOR_HEIGHT		0x08	/* Active height */
#define VSOC_SENSOR_FMT			0x0A	/* Media bus format code (low 16) */

/* Image controls */
#define VSOC_SENSOR_EXPOSURE		0x0C	/* Exposure time (0-65535) */
#define VSOC_SENSOR_GAIN		0x0E	/* Analog gain (0-255) */
#define VSOC_SENSOR_DGAIN		0x10	/* Digital gain (0-255) */
#define VSOC_SENSOR_HFLIP		0x12	/* Horizontal flip (0/1) */
#define VSOC_SENSOR_VFLIP		0x14	/* Vertical flip (0/1) */
#define VSOC_SENSOR_TESTPAT		0x16	/* Test pattern (0=off, 1-4=patterns) */

/* Status */
#define VSOC_SENSOR_STATUS		0x18	/* RO: sensor status */
#define VSOC_SENSOR_STATUS_READY	BIT(0)
#define VSOC_SENSOR_STATUS_STREAMING	BIT(1)

/* Hardware constants */
#define VSOC_SENSOR_CHIP_ID_VAL		0x3000
#define VSOC_SENSOR_CHIP_REV_VAL	0x01
#define VSOC_SENSOR_REG_COUNT		16	/* 16 word registers */

#define VSOC_SENSOR_DEF_WIDTH		1920
#define VSOC_SENSOR_DEF_HEIGHT		1080
#define VSOC_SENSOR_MIN_WIDTH		160
#define VSOC_SENSOR_MAX_WIDTH		3840
#define VSOC_SENSOR_MIN_HEIGHT		120
#define VSOC_SENSOR_MAX_HEIGHT		2160
#define VSOC_SENSOR_STEP		16

/* Default control values */
#define VSOC_SENSOR_DEF_EXPOSURE	1000
#define VSOC_SENSOR_MAX_EXPOSURE	65535
#define VSOC_SENSOR_DEF_GAIN		64
#define VSOC_SENSOR_MAX_GAIN		255

/* ====================================================================
 * Section 2: CSI-2 Receiver Registers (MMIO, 32-bit)
 *
 * Accessed via ioread32/iowrite32 on mapped register block.
 * ==================================================================== */

#define VSOC_CSI2_CTRL			0x000
#define VSOC_CSI2_CTRL_ENABLE		BIT(0)
#define VSOC_CSI2_CTRL_STREAM		BIT(1)

#define VSOC_CSI2_STATUS		0x004
#define VSOC_CSI2_STATUS_READY		BIT(0)
#define VSOC_CSI2_STATUS_STREAMING	BIT(1)
#define VSOC_CSI2_STATUS_LINK_UP	BIT(2)

#define VSOC_CSI2_FMT			0x008	/* Media bus format code */
#define VSOC_CSI2_WIDTH			0x00C
#define VSOC_CSI2_HEIGHT		0x010
#define VSOC_CSI2_LANES			0x014	/* Number of data lanes (1-4) */

#define VSOC_CSI2_INT_MASK		0x018
#define VSOC_CSI2_INT_STATUS		0x01C
#define VSOC_CSI2_INT_FRAME_START	BIT(0)
#define VSOC_CSI2_INT_FRAME_END		BIT(1)
#define VSOC_CSI2_INT_ERROR		BIT(2)

#define VSOC_CSI2_REG_COUNT		16	/* 16 registers */

/* ====================================================================
 * Section 3: ISP Registers (MMIO, 32-bit)
 *
 * The ISP performs format conversion (Bayer → RGB) and image
 * processing (brightness, contrast).
 * ==================================================================== */

#define VSOC_ISP_CTRL			0x000
#define VSOC_ISP_CTRL_ENABLE		BIT(0)
#define VSOC_ISP_CTRL_STREAM		BIT(1)
#define VSOC_ISP_CTRL_BYPASS		BIT(2)	/* Bypass processing */

#define VSOC_ISP_STATUS			0x004
#define VSOC_ISP_STATUS_READY		BIT(0)
#define VSOC_ISP_STATUS_STREAMING	BIT(1)
#define VSOC_ISP_STATUS_BUSY		BIT(2)

#define VSOC_ISP_IN_FMT			0x008	/* Input media bus format */
#define VSOC_ISP_OUT_FMT		0x00C	/* Output media bus format */
#define VSOC_ISP_WIDTH			0x010
#define VSOC_ISP_HEIGHT			0x014

/* ISP processing controls */
#define VSOC_ISP_BRIGHTNESS		0x018	/* 0-255, default 128 */
#define VSOC_ISP_CONTRAST		0x01C	/* 0-255, default 128 */

#define VSOC_ISP_INT_MASK		0x020
#define VSOC_ISP_INT_STATUS		0x024
#define VSOC_ISP_INT_FRAME_DONE		BIT(0)
#define VSOC_ISP_INT_ERROR		BIT(1)

#define VSOC_ISP_REG_COUNT		16	/* 16 registers */

/* ====================================================================
 * Section 4: DMA Engine Registers (MMIO, 32-bit)
 *
 * The DMA engine uses a descriptor ring identical to the VCAM-2000.
 * It writes processed frames from the ISP into system memory.
 * ==================================================================== */

#define VSOC_DMA_CTRL			0x000
#define VSOC_DMA_CTRL_ENABLE		BIT(0)
#define VSOC_DMA_CTRL_STREAM		BIT(1)
#define VSOC_DMA_CTRL_RING_ENABLE	BIT(2)

#define VSOC_DMA_STATUS			0x004
#define VSOC_DMA_STATUS_READY		BIT(0)
#define VSOC_DMA_STATUS_STREAMING	BIT(1)

#define VSOC_DMA_INT_MASK		0x008
#define VSOC_DMA_INT_STATUS		0x00C
#define VSOC_DMA_INT_FRAME_DONE		BIT(0)
#define VSOC_DMA_INT_ERROR		BIT(1)
#define VSOC_DMA_INT_OVERFLOW		BIT(2)
#define VSOC_DMA_INT_ALL		(VSOC_DMA_INT_FRAME_DONE | \
					 VSOC_DMA_INT_ERROR | \
					 VSOC_DMA_INT_OVERFLOW)

#define VSOC_DMA_FMT_WIDTH		0x010
#define VSOC_DMA_FMT_HEIGHT		0x014
#define VSOC_DMA_FMT_STRIDE		0x018
#define VSOC_DMA_FMT_FRAMESIZE		0x01C

#define VSOC_DMA_BUF_RING_ADDR		0x100
#define VSOC_DMA_BUF_RING_SIZE		0x104
#define VSOC_DMA_BUF_RING_HEAD		0x108
#define VSOC_DMA_BUF_RING_TAIL		0x10C

#define VSOC_DMA_FRAME_COUNT		0x110
#define VSOC_DMA_FRAME_TS_LO		0x114
#define VSOC_DMA_FRAME_TS_HI		0x118

#define VSOC_DMA_STATS_FRAMES		0x200
#define VSOC_DMA_STATS_BYTES		0x204
#define VSOC_DMA_STATS_ERRORS		0x208
#define VSOC_DMA_STATS_DROPPED		0x20C

#define VSOC_DMA_REG_COUNT		144

/* ====================================================================
 * Section 5: Buffer Descriptor Format (16 bytes)
 *
 * Same format as VCAM-2000 for familiarity.
 * ==================================================================== */

struct vsoc_hw_desc {
	u32 addr_lo;	/* Buffer address (low 32 bits)  */
	u32 addr_hi;	/* Buffer address (high 32 bits) */
	u32 size;	/* Buffer size in bytes          */
	u32 flags;	/* Ownership and status flags    */
};

#define VSOC_DESC_OWN		BIT(31)		/* 1 = owned by hardware */
#define VSOC_DESC_DONE		BIT(30)		/* Frame written to buffer */
#define VSOC_DESC_ERROR		BIT(29)		/* Error on this frame */
#define VSOC_DESC_SEQ_MASK	0x0000FFFF	/* Sequence number */

/* ====================================================================
 * Section 6: Hardware Constants
 * ==================================================================== */

#define VSOC_HW_FRAME_MS	33	/* ~30 fps default */
#define VSOC_HW_RING_MAX	16

/* Device names for platform bus matching */
#define VSOC_BRIDGE_DEV_NAME	"vsoc_bridge"
#define VSOC_CSI2_DEV_NAME	"vsoc_csi2"
#define VSOC_ISP_DEV_NAME	"vsoc_isp"

/* Hardware pixel format codes (V4L2 fourcc) */
#define VSOC_HW_FMT_RGB24	0x33424752 /* v4l2_fourcc('R','G','B','3') */

/* ====================================================================
 * Section 7: Platform Module Interface
 *
 * Exported by soc_hw_platform.ko for use by driver modules.
 *
 * Load order:
 *   sudo insmod soc/hw/soc_hw_platform.ko
 *   sudo insmod soc/partN/vsoc_*.ko
 * ==================================================================== */

/* I2C adapter (for sensor subdev driver) */
extern struct i2c_adapter *vsoc_hw_get_i2c_adapter(void);

/* MMIO register maps */
extern void __iomem *vsoc_hw_map_csi2_regs(void);
extern void __iomem *vsoc_hw_map_isp_regs(void);
extern void __iomem *vsoc_hw_map_dma_regs(void);

/* Unmap functions */
extern void vsoc_hw_unmap_csi2_regs(void);
extern void vsoc_hw_unmap_isp_regs(void);
extern void vsoc_hw_unmap_dma_regs(void);

/* DMA IRQ */
extern int vsoc_hw_get_dma_irq(void);

/* DMA buffer ring management */
extern int  vsoc_hw_set_buf_ring(void *ring_va, u32 count);
extern void vsoc_hw_clear_buf_ring(void);

/* Streaming notification */
extern void vsoc_hw_notify_stream(int on);

/* Platform devices (for logging) */
extern struct platform_device *vsoc_hw_get_bridge_pdev(void);
extern struct platform_device *vsoc_hw_get_csi2_pdev(void);
extern struct platform_device *vsoc_hw_get_isp_pdev(void);

#endif /* SOC_HW_INTERFACE_H */
