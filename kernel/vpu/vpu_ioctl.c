// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VPU ioctl Handler
 * Copyright (C) 2026 StrayLight Systems
 *
 * Dispatches ALLOC / FREE / MAP / QUERY / EXPORT_DMA / IMPORT_DMA ioctls.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

#include "vpu.h"

/* ---- helpers ----------------------------------------------------------- */

static struct vpu_allocation *vpu_find_alloc(struct vpu_device *vdev,
					     __u64 handle)
{
	struct vpu_allocation *alloc;

	list_for_each_entry(alloc, &vdev->alloc_list, list) {
		if (alloc->handle == handle)
			return alloc;
	}
	return NULL;
}

/* ---- ALLOC ------------------------------------------------------------- */

static long vpu_ioctl_alloc(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_alloc_req req;
	struct vpu_allocation *alloc;
	void *cpu_addr;
	dma_addr_t dma_addr;
	int order, block_idx, ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.size == 0)
		return -EINVAL;

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return -ENOMEM;

	ret = vpu_slab_alloc(vdev, req.size, &cpu_addr, &dma_addr,
			     &order, &block_idx);
	if (ret) {
		kfree(alloc);
		return ret;
	}

	mutex_lock(&vdev->alloc_lock);

	alloc->handle    = vdev->next_handle++;
	alloc->order     = order;
	alloc->block_idx = block_idx;
	alloc->size      = vpu_slab_sizes[order];
	alloc->cpu_addr  = cpu_addr;
	alloc->dma_addr  = dma_addr;
	alloc->flags     = req.flags;
	alloc->vdev      = vdev;
	alloc->dmabuf    = NULL;

	/* Assign a GPU VA — simple bump allocator */
	alloc->gpu_addr  = vdev->next_gpu_addr;
	vdev->next_gpu_addr += alloc->size;

	list_add_tail(&alloc->list, &vdev->alloc_list);

	atomic64_add(alloc->size, &vdev->total_allocated);
	atomic64_inc(&vdev->active_allocs);

	mutex_unlock(&vdev->alloc_lock);

	/* Return results to userspace */
	req.handle   = alloc->handle;
	req.gpu_addr = alloc->gpu_addr;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- FREE -------------------------------------------------------------- */

static long vpu_ioctl_free(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_free_req req;
	struct vpu_allocation *alloc;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&vdev->alloc_lock);

	alloc = vpu_find_alloc(vdev, req.handle);
	if (!alloc) {
		mutex_unlock(&vdev->alloc_lock);
		return -ENOENT;
	}

	list_del(&alloc->list);
	mutex_unlock(&vdev->alloc_lock);

	vpu_slab_free(vdev, alloc->order, alloc->block_idx);
	atomic64_add(alloc->size, &vdev->total_freed);
	atomic64_dec(&vdev->active_allocs);

	kfree(alloc);
	return 0;
}

/* ---- MAP --------------------------------------------------------------- */

static long vpu_ioctl_map(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_map_req req;
	struct vpu_allocation *alloc;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&vdev->alloc_lock);

	alloc = vpu_find_alloc(vdev, req.handle);
	if (!alloc) {
		mutex_unlock(&vdev->alloc_lock);
		return -ENOENT;
	}

	/*
	 * The mmap offset is the DMA address — vpu_mmap() matches it
	 * to find the correct allocation.
	 */
	req.offset = alloc->dma_addr;
	req.size   = alloc->size;

	mutex_unlock(&vdev->alloc_lock);

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- QUERY ------------------------------------------------------------- */

static long vpu_ioctl_query(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_query_req req;
	struct vpu_allocation *alloc;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&vdev->alloc_lock);

	alloc = vpu_find_alloc(vdev, req.handle);
	if (!alloc) {
		mutex_unlock(&vdev->alloc_lock);
		return -ENOENT;
	}

	req.size     = alloc->size;
	req.order    = alloc->order;
	req.flags    = alloc->flags;
	req.gpu_addr = alloc->gpu_addr;

	mutex_unlock(&vdev->alloc_lock);

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- EXPORT DMA -------------------------------------------------------- */

static long vpu_ioctl_export_dma(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_dma_export_req req;
	struct vpu_allocation *alloc;
	int fd, ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&vdev->alloc_lock);
	alloc = vpu_find_alloc(vdev, req.handle);
	mutex_unlock(&vdev->alloc_lock);

	if (!alloc)
		return -ENOENT;

	ret = vpu_dma_buf_export(alloc, req.flags, &fd);
	if (ret)
		return ret;

	req.fd = fd;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- IMPORT DMA -------------------------------------------------------- */

static long vpu_ioctl_import_dma(struct vpu_device *vdev, unsigned long arg)
{
	struct vpu_dma_import_req req;
	struct vpu_allocation *alloc;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	ret = vpu_dma_buf_import(vdev, req.fd, req.flags, &alloc);
	if (ret)
		return ret;

	req.handle = alloc->handle;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- dispatch ---------------------------------------------------------- */

long vpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct vpu_device *vdev = filp->private_data;

	switch (cmd) {
	case VPU_IOC_ALLOC:
		return vpu_ioctl_alloc(vdev, arg);
	case VPU_IOC_FREE:
		return vpu_ioctl_free(vdev, arg);
	case VPU_IOC_MAP:
		return vpu_ioctl_map(vdev, arg);
	case VPU_IOC_QUERY:
		return vpu_ioctl_query(vdev, arg);
	case VPU_IOC_EXPORT_DMA:
		return vpu_ioctl_export_dma(vdev, arg);
	case VPU_IOC_IMPORT_DMA:
		return vpu_ioctl_import_dma(vdev, arg);
	default:
		return -ENOTTY;
	}
}
