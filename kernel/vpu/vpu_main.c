// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VPU Main Module
 * Copyright (C) 2026 StrayLight Systems
 *
 * Module entry/exit, misc-device registration, file_operations.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "vpu.h"
#include "vpu_mgpu.h"

#define VPU_DEV_NAME    "straylight-vpu"
#define VPU_MGPU_HANDLE_FLAG (1ULL << 63)

static struct vpu_device *g_vdev;
static struct vpu_mgpu_context *g_mgpu;

/* ---- file operations --------------------------------------------------- */

static int vpu_open(struct inode *inode, struct file *filp)
{
	filp->private_data = g_vdev;
	pr_info("straylight-vpu: device opened by pid %d\n", current->pid);
	return 0;
}

static int vpu_release(struct inode *inode, struct file *filp)
{
	pr_info("straylight-vpu: device closed by pid %d\n", current->pid);
	return 0;
}

static int vpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct vpu_device *vdev = filp->private_data;
	struct vpu_allocation *alloc;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	int found = 0;

	mutex_lock(&vdev->alloc_lock);
	list_for_each_entry(alloc, &vdev->alloc_list, list) {
		if (alloc->dma_addr == offset && alloc->size >= size) {
			found = 1;
			break;
		}
	}
	mutex_unlock(&vdev->alloc_lock);

	if (!found)
		return -EINVAL;

	/* Mark the VMA as IO memory — prevents caching issues */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

	if (remap_pfn_range(vma, vma->vm_start,
			    alloc->dma_addr >> PAGE_SHIFT,
			    size, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static bool vpu_is_mgpu_command(unsigned int cmd)
{
	switch (cmd) {
	case VPU_IOC_ENUM_GPUS:
	case VPU_IOC_ALLOC_ON:
	case VPU_IOC_P2P_COPY:
	case VPU_IOC_MIRROR:
	case VPU_IOC_GPU_STATS:
		return true;
	default:
		return false;
	}
}

static bool vpu_is_mgpu_handle(__u64 handle)
{
	return (handle & VPU_MGPU_HANDLE_FLAG) != 0;
}

static long vpu_dispatch_free(struct file *filp, unsigned long arg)
{
	struct vpu_free_req req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (g_mgpu && vpu_is_mgpu_handle(req.handle))
		return vpu_mgpu_free_handle(g_mgpu, req.handle);

	return vpu_ioctl(filp, VPU_IOC_FREE, arg);
}

static long vpu_dispatch_query(struct file *filp, unsigned long arg)
{
	struct vpu_query_req req;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (!g_mgpu || !vpu_is_mgpu_handle(req.handle))
		return vpu_ioctl(filp, VPU_IOC_QUERY, arg);

	ret = vpu_mgpu_query_handle(g_mgpu, req.handle, &req);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long vpu_dispatch_export_dma(struct file *filp, unsigned long arg)
{
	struct vpu_dma_export_req req;
	int fd, ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (!g_mgpu || !vpu_is_mgpu_handle(req.handle))
		return vpu_ioctl(filp, VPU_IOC_EXPORT_DMA, arg);

	ret = vpu_mgpu_export_dma(g_mgpu, req.handle, req.flags, &fd);
	if (ret)
		return ret;

	req.fd = fd;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long vpu_unlocked_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	if (g_mgpu && vpu_is_mgpu_command(cmd))
		return vpu_mgpu_ioctl(g_mgpu, cmd, arg);

	switch (cmd) {
	case VPU_IOC_FREE:
		return vpu_dispatch_free(filp, arg);
	case VPU_IOC_QUERY:
		return vpu_dispatch_query(filp, arg);
	case VPU_IOC_EXPORT_DMA:
		return vpu_dispatch_export_dma(filp, arg);
	default:
		return vpu_ioctl(filp, cmd, arg);
	}
}

static const struct file_operations vpu_fops = {
	.owner          = THIS_MODULE,
	.open           = vpu_open,
	.release        = vpu_release,
	.unlocked_ioctl = vpu_unlocked_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.mmap           = vpu_mmap,
};

/* ---- module init / exit ------------------------------------------------ */

static int __init vpu_module_init(void)
{
	int ret;

	g_vdev = kzalloc(sizeof(*g_vdev), GFP_KERNEL);
	if (!g_vdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&g_vdev->alloc_list);
	mutex_init(&g_vdev->alloc_lock);
	atomic64_set(&g_vdev->total_allocated, 0);
	atomic64_set(&g_vdev->total_freed, 0);
	atomic64_set(&g_vdev->active_allocs, 0);
	g_vdev->next_handle   = 1;
	g_vdev->next_gpu_addr = 0x100000000ULL; /* GPU VA base at 4 GiB */

	/* Initialise slab pools */
	ret = vpu_slab_init(g_vdev);
	if (ret) {
		pr_err("straylight-vpu: slab init failed (%d)\n", ret);
		goto err_slab;
	}

	/* Register misc device */
	g_vdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	g_vdev->miscdev.name  = VPU_DEV_NAME;
	g_vdev->miscdev.fops  = &vpu_fops;

	ret = misc_register(&g_vdev->miscdev);
	if (ret) {
		pr_err("straylight-vpu: misc_register failed (%d)\n", ret);
		goto err_misc;
	}
	g_vdev->dev = g_vdev->miscdev.this_device;

	/* sysfs attributes */
	ret = vpu_sysfs_init(g_vdev);
	if (ret) {
		pr_warn("straylight-vpu: sysfs init failed (%d), continuing\n", ret);
		/* non-fatal — device still works */
	}

	g_mgpu = kzalloc(sizeof(*g_mgpu), GFP_KERNEL);
	if (g_mgpu) {
		mutex_init(&g_mgpu->lock);
		ret = vpu_mgpu_discover(g_mgpu);
		if (ret) {
			pr_warn("straylight-vpu: mgpu discovery failed (%d), "
				"continuing with base VPU ABI\n", ret);
			mutex_destroy(&g_mgpu->lock);
			kfree(g_mgpu);
			g_mgpu = NULL;
		} else if (g_vdev->sysfs_kobj) {
			ret = vpu_mgpu_sysfs_init(g_mgpu, g_vdev->sysfs_kobj);
			if (ret)
				pr_warn("straylight-vpu: mgpu sysfs init "
					"failed (%d), ioctls remain available\n",
					ret);
		}
	} else {
		pr_warn("straylight-vpu: mgpu context allocation failed, "
			"continuing with base VPU ABI\n");
	}

	pr_info("straylight-vpu: module loaded — %d slab orders\n",
		VPU_SLAB_ORDER_COUNT);
	return 0;

err_misc:
	vpu_slab_destroy(g_vdev);
err_slab:
	kfree(g_vdev);
	g_vdev = NULL;
	return ret;
}

static void __exit vpu_module_exit(void)
{
	struct vpu_allocation *alloc, *tmp;

	if (!g_vdev)
		return;

	if (g_mgpu) {
		vpu_mgpu_destroy(g_mgpu);
		mutex_destroy(&g_mgpu->lock);
		kfree(g_mgpu);
		g_mgpu = NULL;
	}

	vpu_sysfs_cleanup(g_vdev);

	/* Free any leaked allocations */
	mutex_lock(&g_vdev->alloc_lock);
	list_for_each_entry_safe(alloc, tmp, &g_vdev->alloc_list, list) {
		pr_warn("straylight-vpu: leaked alloc handle=%llu size=%zu\n",
			alloc->handle, alloc->size);
		vpu_slab_free(g_vdev, alloc->order, alloc->block_idx);
		list_del(&alloc->list);
		kfree(alloc);
	}
	mutex_unlock(&g_vdev->alloc_lock);

	misc_deregister(&g_vdev->miscdev);
	vpu_slab_destroy(g_vdev);
	kfree(g_vdev);
	g_vdev = NULL;

	pr_info("straylight-vpu: module unloaded\n");
}

module_init(vpu_module_init);
module_exit(vpu_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StrayLight Systems");
MODULE_DESCRIPTION("StrayLight VPU GPU Memory Slab Allocator");
MODULE_VERSION("1.0.0");
