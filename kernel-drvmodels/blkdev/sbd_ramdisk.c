/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * Simple Block Device (SBD) - RAM Disk Driver
 *
 * File I/O Path (very high level):
 *   userspace read/write → VFS → filesystem builds bios → submit_bio()
 *   → block core (splitting/merging/limits) → blk-mq → queue_rq()
 *   → driver performs I/O (DMA/memcpy) → complete → bios endio → FS/VFS
 *
 * This RAM-disk device stores data in kernel memory (vmalloc), so "I/O" is a
 * memcpy rather than hardware programming. This keeps the driver small and the
 * blk-mq concepts clear.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>
#include <linux/printk.h>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

/*
 * Major number for the block device. 0 asks the kernel to auto-assign one.
 * The actual major is written back here by register_blkdev().
 */
static int dev_major = 0;

/*
 * Per-device state for this simple driver. Real drivers often carry more
 * fields, but these are sufficient to demonstrate blk-mq usage.
 *
 * - capacity: number of 512-byte sectors
 * - data:     vmalloc-backed buffer acting as the storage medium
 * - tag_set:  blk-mq configuration shared across the device's HW queues
 * - queue:    request queue backing the gendisk
 * - gdisk:    generic disk object (visible to the rest of the kernel/userspace)
 */
struct block_dev {
    sector_t capacity;
    u8 *data;
    struct blk_mq_tag_set tag_set;
    struct request_queue *queue;
    struct gendisk *gdisk;
};

/* Single device instance for this module. */
static struct block_dev *block_device = NULL;

int do_request(struct request *rq, unsigned int *nr_bytes);

/*
 * Block device file operations
 *
 * Modern kernels pass struct gendisk* to .open/.release. These hooks are
 * invoked when userspace opens/closes the device node (/dev/sbd). For a RAM
 * disk we do not need to enforce permissions or track per-open state here.
 */
static int sbd_open(struct gendisk *gdisk, blk_mode_t mode)
{
    /* Keep at debug to avoid log spam during heavy open/close */
    pr_debug(">>> sbd_open\n");
    return 0;
}

static void sbd_release(struct gendisk *gdisk)
{
    pr_debug(">>> sbd_release\n");
}

/*
 * Optional ioctl handler for block devices. This simple driver does not
 * implement custom ioctls and returns -ENOTTY for all commands.
 */
static int sbd_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned cmd, unsigned long arg)
{
    pr_debug("ioctl cmd 0x%08x\n", cmd);
    return -ENOTTY;
}

/*
 * The operations table exported via gendisk->fops so the block core can call
 * us for open/close/ioctl. owner ensures module refcounting.
 */
static struct block_device_operations sbd_ops = {
    .owner   = THIS_MODULE,
    .open    = sbd_open,
    .release = sbd_release,
    .ioctl   = sbd_ioctl,
};

/*
 * do_request(): process a single request by iterating its segments
 *
 * - rq_for_each_segment() walks all bio_vecs across all bios in the request
 * - For each segment we compute the RAM buffer address and memcpy() either
 *   into the device (WRITE) or out of it (READ)
 * - We track "pos" in bytes using rq's starting sector and SECTOR_SIZE
 * - We ensure we never copy past the device capacity (bounds check)
 * - nr_bytes is incremented with the number of bytes we actually handled
 *
 */
int do_request(struct request *rq, unsigned int *nr_bytes)
{
    int ret = 0;
    struct bio_vec bvec;
    struct req_iterator iter;
    struct block_dev *dev = rq->q->queuedata;
    loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;       /* request offset in bytes */
    loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT);
    *nr_bytes = 0;

    rq_for_each_segment(bvec, rq, iter) {
        unsigned long b_len = bvec.bv_len;             /* bytes in this segment */
        void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

        /* Prevent out-of-bounds accesses if request overflows device end. */
        if ((pos + b_len) > dev_size)
            b_len = (unsigned long)(dev_size - pos);

        if (rq_data_dir(rq) == WRITE) {
            /* WRITE: copy from memory provided by the request into our disk */
            memcpy(dev->data + pos, b_buf, b_len);
        } else {
            /* READ: copy from our disk into the request's memory */
            memcpy(b_buf, dev->data + pos, b_len);
        }

        pos += b_len;
        *nr_bytes += b_len;  /* report progress for blk_update_request() */
    }
    return ret;
}

/*
 * queue_rq(): blk-mq dispatch entry point
 *
 * The block layer calls this to submit a prepared request on a specific
 * hardware queue context (hctx). For a RAM disk, we handle it synchronously;
 * hardware drivers might program DMA and complete later from an interrupt.
 */
static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;

    /* Mark the request as started (accounting/timeout handling). */
    blk_mq_start_request(rq);

    if (do_request(rq, &nr_bytes) != 0)
        status = BLK_STS_IOERR;

    /* Let the core know how many bytes were processed and finish the request. */
    if (blk_update_request(rq, status, nr_bytes))
        BUG(); /* Should not happen for a fully handled request */

    __blk_mq_end_request(rq, status);
    return status;
}

/* The blk-mq operations vector: queue_rq is mandatory; others are optional. */
static struct blk_mq_ops mq_ops = {
    .queue_rq = queue_rq,
};

/*
 * Module initialization: create a single blk-mq-backed disk named "sbd"
 *
 * Steps:
 *  1) Register a block device major
 *  2) Allocate our device struct and RAM buffer
 *  3) Configure and allocate a blk_mq_tag_set
 *  4) Create a gendisk via blk_mq_alloc_disk()
 *  5) Fill disk fields and add it to the system
 */
static int __init myblock_driver_init(void)
{
    int ret = 0;

    /* 1) Reserve a major number (0 = let kernel choose) */
    dev_major = register_blkdev(dev_major, "testblk");
    if (dev_major < 0) {
        pr_err("Failed to register block device\n");
        return dev_major;
    }

    /* 2) Allocate device and its backing store (about 2MB @ 4K pages) */
    block_device = kmalloc(sizeof(struct block_dev), GFP_KERNEL);
    if (!block_device) {
        ret = -ENOMEM;
        goto out_unregister;
    }

    block_device->capacity = (512 * PAGE_SIZE) >> 9; /* sectors */
    block_device->data = vmalloc(block_device->capacity << 9);
    if (!block_device->data) {
        ret = -ENOMEM;
        goto out_free_dev;
    }

    /* 3) Describe blk-mq configuration in tag_set and allocate it */
    pr_info("Initializing tag set\n");
    block_device->tag_set.ops = &mq_ops;        /* driver entry points */
    block_device->tag_set.nr_hw_queues = 1;     /* one HW queue (simple) */
    block_device->tag_set.queue_depth = 128;    /* tags (in-flight requests) */
    block_device->tag_set.numa_node = NUMA_NO_NODE; /* neutral placement */
    block_device->tag_set.cmd_size = 0;         /* per-request driver ctx */
    block_device->tag_set.flags = 0;            /* see BLK_MQ_F_* */
    block_device->tag_set.driver_data = block_device; /* carry our dev ptr */

    ret = blk_mq_alloc_tag_set(&block_device->tag_set);
    if (ret) {
        pr_err("Failed to allocate tag set\n");
        goto out_free_data;
    }

    /* 4) Allocate a disk bound to this tag_set (modern API) */
    block_device->gdisk = blk_mq_alloc_disk(&block_device->tag_set, NULL, block_device);
    if (IS_ERR(block_device->gdisk)) {
        ret = PTR_ERR(block_device->gdisk);
        pr_err("Failed to allocate disk\n");
        goto out_free_tag_set;
    }

    block_device->queue = block_device->gdisk->queue; /* convenience */

    /* 5) Fill out disk metadata and make it live */
    block_device->gdisk->major = dev_major;
    block_device->gdisk->first_minor = 0;
    block_device->gdisk->minors = 1;           /* no partitions */
    block_device->gdisk->fops = &sbd_ops;
    block_device->gdisk->private_data = block_device;

    strncpy(block_device->gdisk->disk_name, "sbd", 4);
    pr_info("Adding disk %s\n", block_device->gdisk->disk_name);

    set_capacity(block_device->gdisk, block_device->capacity);

    ret = add_disk(block_device->gdisk);
    if (ret) {
        pr_err("Failed to add disk\n");
        goto out_cleanup_disk;
    }

    pr_info("Block device driver loaded successfully\n");
    return 0;

out_cleanup_disk:
    put_disk(block_device->gdisk);
out_free_tag_set:
    blk_mq_free_tag_set(&block_device->tag_set);
out_free_data:
    vfree(block_device->data);
out_free_dev:
    kfree(block_device);
out_unregister:
    unregister_blkdev(dev_major, "testblk");
    return ret;
}

/*
 * Module exit: remove disk, free tag set and memory, unregister major.
 * The order is the reverse of initialization; blk-mq frees per-queue
 * allocations when the tag_set is released.
 */
static void __exit myblock_driver_exit(void)
{
    if (block_device->gdisk)
        del_gendisk(block_device->gdisk);

    blk_mq_free_tag_set(&block_device->tag_set);
    vfree(block_device->data);
    unregister_blkdev(dev_major, "testblk");
    kfree(block_device);

    pr_info("Block device driver unloaded\n");
}

module_init(myblock_driver_init);
module_exit(myblock_driver_exit);

MODULE_AUTHOR("Raghu Bhardwaj <raghu@techveda.org>");
MODULE_DESCRIPTION("Simple Block Device - Multi-queue RAM Disk Driver");
MODULE_LICENSE("Dual MIT/GPL");
