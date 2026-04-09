/*
 * Copyright (c) 2024 TECH VEDA
 * Author: Raghu Bharadwaj
 * This code is dual-licensed under the MIT License and GPL v2
 *
 * buf_reader.c - dma-buf importer misc device
 *
 * Receives a dma-buf fd via ioctl and reads the buffer contents
 * using dma_buf_vmap(). Demonstrates the importer side of dma-buf sharing.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>

#include "vcam_expbuf.h"

static struct platform_device *reader_pdev;

static long buf_reader_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct dma_buf *dmabuf;
	struct iosys_map map;
	int fd;
	u32 frame;
	u8 *data;
	int ret;

	if (cmd != BUF_READER_IOCTL_IMPORT)
		return -ENOTTY;

	if (copy_from_user(&fd, (int __user *)arg, sizeof(fd)))
		return -EFAULT;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		pr_err("buf_reader: dma_buf_get(%d) failed: %ld\n",
		       fd, PTR_ERR(dmabuf));
		return PTR_ERR(dmabuf);
	}

	pr_info("buf_reader: imported dma_buf fd=%d, size=%zu\n",
		fd, dmabuf->size);

	ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
	if (ret) {
		pr_err("buf_reader: begin_cpu_access failed: %d\n", ret);
		goto put;
	}

	ret = dma_buf_vmap(dmabuf, &map);
	if (ret) {
		pr_err("buf_reader: vmap failed: %d\n", ret);
		goto end_access;
	}

	data = map.vaddr;
	frame = *(u32 *)data;
	pr_info("buf_reader: frame=%u, pixel@4: R=%u G=%u B=%u\n",
		frame, data[12], data[13], data[14]);

	dma_buf_vunmap(dmabuf, &map);

end_access:
	dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
put:
	dma_buf_put(dmabuf);
	return ret;
}

static const struct file_operations buf_reader_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = buf_reader_ioctl,
};

static struct miscdevice buf_reader_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "buf_reader",
	.fops  = &buf_reader_fops,
};

static int __init buf_reader_init(void)
{
	int ret;

	reader_pdev = platform_device_register_simple("buf_reader", -1,
						      NULL, 0);
	if (IS_ERR(reader_pdev))
		return PTR_ERR(reader_pdev);

	ret = misc_register(&buf_reader_misc);
	if (ret) {
		platform_device_unregister(reader_pdev);
		return ret;
	}

	pr_info("buf_reader: registered /dev/buf_reader\n");
	return 0;
}

static void __exit buf_reader_exit(void)
{
	misc_deregister(&buf_reader_misc);
	platform_device_unregister(reader_pdev);
	pr_info("buf_reader: unregistered\n");
}

module_init(buf_reader_init);
module_exit(buf_reader_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raghu Bharadwaj");
MODULE_DESCRIPTION("dma-buf importer misc device");
MODULE_IMPORT_NS("DMA_BUF");
