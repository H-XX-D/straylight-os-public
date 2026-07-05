// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VPU DMA-BUF Export / Import
 * Copyright (C) 2026 StrayLight Systems
 *
 * Implements dma_buf_ops so VPU allocations can be shared with other
 * subsystems (GPU, display, video encoder, etc.) via dma-buf fds.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/file.h>

#include "vpu.h"

/* ---- dma_buf_ops implementation ---------------------------------------- */

struct vpu_dma_attachment {
	struct sg_table         sgt;
	struct vpu_allocation   *alloc;
	enum dma_data_direction dir;
};

static int vpu_dmabuf_attach(struct dma_buf *dbuf,
			     struct dma_buf_attachment *attach)
{
	struct vpu_dma_attachment *a;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	a->alloc = dbuf->priv;
	attach->priv = a;
	return 0;
}

static void vpu_dmabuf_detach(struct dma_buf *dbuf,
			      struct dma_buf_attachment *attach)
{
	struct vpu_dma_attachment *a = attach->priv;

	kfree(a);
	attach->priv = NULL;
}

static struct sg_table *vpu_dmabuf_map(struct dma_buf_attachment *attach,
				       enum dma_data_direction dir)
{
	struct vpu_dma_attachment *a = attach->priv;
	struct vpu_allocation *alloc = a->alloc;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned long nr_pages;
	unsigned long i;
	struct page *page;
	void *addr;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	nr_pages = alloc->size >> PAGE_SHIFT;
	if (nr_pages == 0)
		nr_pages = 1;

	ret = sg_alloc_table(sgt, nr_pages, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	/* Build scatter-gather list from vmalloc'd pages */
	addr = alloc->cpu_addr;
	for_each_sg(sgt->sgl, sg, nr_pages, i) {
		page = vmalloc_to_page(addr);
		if (!page) {
			sg_free_table(sgt);
			kfree(sgt);
			return ERR_PTR(-EFAULT);
		}
		sg_set_page(sg, page, PAGE_SIZE, 0);
		addr += PAGE_SIZE;
	}

	ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	a->dir = dir;
	return sgt;
}

static void vpu_dmabuf_unmap(struct dma_buf_attachment *attach,
			     struct sg_table *sgt,
			     enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static int vpu_dmabuf_mmap(struct dma_buf *dbuf, struct vm_area_struct *vma)
{
	struct vpu_allocation *alloc = dbuf->priv;
	unsigned long nr_pages;
	unsigned long i;
	unsigned long uaddr;
	void *addr;
	struct page *page;

	nr_pages = alloc->size >> PAGE_SHIFT;
	if (nr_pages == 0)
		nr_pages = 1;

	uaddr = vma->vm_start;
	addr  = alloc->cpu_addr;

	for (i = 0; i < nr_pages && uaddr < vma->vm_end; i++) {
		page = vmalloc_to_page(addr);
		if (!page)
			return -EFAULT;

		if (vm_insert_page(vma, uaddr, page))
			return -EFAULT;

		uaddr += PAGE_SIZE;
		addr  += PAGE_SIZE;
	}

	return 0;
}

static int vpu_dmabuf_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct vpu_allocation *alloc = dbuf->priv;

	iosys_map_set_vaddr(map, alloc->cpu_addr);
	return 0;
}

static void vpu_dmabuf_vunmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	iosys_map_clear(map);
}

static void vpu_dmabuf_release(struct dma_buf *dbuf)
{
	struct vpu_allocation *alloc = dbuf->priv;

	/*
	 * Clear the back-pointer so that when the allocation itself is
	 * freed it does not try to tear down the dma-buf again.
	 */
	alloc->dmabuf = NULL;
}

static const struct dma_buf_ops vpu_dmabuf_ops = {
	.attach         = vpu_dmabuf_attach,
	.detach         = vpu_dmabuf_detach,
	.map_dma_buf    = vpu_dmabuf_map,
	.unmap_dma_buf  = vpu_dmabuf_unmap,
	.mmap           = vpu_dmabuf_mmap,
	.vmap           = vpu_dmabuf_vmap,
	.vunmap         = vpu_dmabuf_vunmap,
	.release        = vpu_dmabuf_release,
};

/* ---- public API -------------------------------------------------------- */

int vpu_dma_buf_export(struct vpu_allocation *alloc, int flags, int *fd_out)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dbuf;
	int fd;

	if (alloc->dmabuf) {
		/* Already exported — return existing fd */
		fd = dma_buf_fd(alloc->dmabuf, flags);
		if (fd < 0)
			return fd;
		*fd_out = fd;
		return 0;
	}

	exp_info.ops   = &vpu_dmabuf_ops;
	exp_info.size  = alloc->size;
	exp_info.flags = flags;
	exp_info.priv  = alloc;

	dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(dbuf))
		return PTR_ERR(dbuf);

	fd = dma_buf_fd(dbuf, flags);
	if (fd < 0) {
		dma_buf_put(dbuf);
		return fd;
	}

	alloc->dmabuf = dbuf;
	*fd_out = fd;
	return 0;
}

int vpu_dma_buf_import(struct vpu_device *vdev, int fd, int flags,
		       struct vpu_allocation **alloc_out)
{
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct vpu_allocation *alloc;
	void *cpu_addr;
	size_t size;
	struct iosys_map map;
	int ret;

	dbuf = dma_buf_get(fd);
	if (IS_ERR(dbuf))
		return PTR_ERR(dbuf);

	size = dbuf->size;

	/* If it's one of our own buffers, just return the existing alloc */
	if (dbuf->ops == &vpu_dmabuf_ops) {
		alloc = dbuf->priv;
		dma_buf_put(dbuf);
		*alloc_out = alloc;
		return 0;
	}

	/* Foreign buffer — attach and vmap */
	attach = dma_buf_attach(dbuf, vdev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto err_put;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	ret = dma_buf_vmap(dbuf, &map);
	if (ret)
		goto err_unmap;

	cpu_addr = map.vaddr;

	/* Create a tracking allocation */
	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc) {
		ret = -ENOMEM;
		goto err_vunmap;
	}

	mutex_lock(&vdev->alloc_lock);

	alloc->handle    = vdev->next_handle++;
	alloc->order     = -1; /* not from our slab */
	alloc->block_idx = -1;
	alloc->size      = size;
	alloc->cpu_addr  = cpu_addr;
	alloc->dma_addr  = sg_dma_address(sgt->sgl);
	alloc->flags     = flags;
	alloc->vdev      = vdev;
	alloc->dmabuf    = dbuf;
	alloc->gpu_addr  = vdev->next_gpu_addr;
	vdev->next_gpu_addr += size;

	list_add_tail(&alloc->list, &vdev->alloc_list);
	atomic64_inc(&vdev->active_allocs);

	mutex_unlock(&vdev->alloc_lock);

	*alloc_out = alloc;
	return 0;

err_vunmap:
	dma_buf_vunmap(dbuf, &map);
err_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dbuf, attach);
err_put:
	dma_buf_put(dbuf);
	return ret;
}

MODULE_IMPORT_NS(DMA_BUF);
