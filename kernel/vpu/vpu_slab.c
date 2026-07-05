// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VPU Slab Allocator
 * Copyright (C) 2026 StrayLight Systems
 *
 * 5-order slab allocator: 4KB / 64KB / 1MB / 16MB / 256MB
 * Bitmap-tracked block allocation within vmalloc'd pools.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bitmap.h>
#include <linux/mutex.h>
#include <linux/dma-mapping.h>
#include <linux/log2.h>

#include "vpu.h"

/*
 * Per-order pool sizing.  We cap each pool to a reasonable virtual
 * reservation so the module loads on machines without huge RAM.
 *
 * order 0 (4K)   : 1024 blocks  =   4 MiB
 * order 1 (64K)  :  256 blocks  =  16 MiB
 * order 2 (1M)   :   64 blocks  =  64 MiB
 * order 3 (16M)  :   16 blocks  = 256 MiB
 * order 4 (256M) :    4 blocks  =   1 GiB
 */
static const unsigned int slab_block_counts[VPU_SLAB_ORDER_COUNT] = {
	1024, 256, 64, 16, 4,
};

/* ---- init / destroy ---------------------------------------------------- */

int vpu_slab_init(struct vpu_device *vdev)
{
	int i;

	for (i = 0; i < VPU_SLAB_ORDER_COUNT; i++) {
		struct vpu_slab *slab = &vdev->slabs[i];
		size_t pool_bytes;

		slab->block_size  = vpu_slab_sizes[i];
		slab->nr_blocks   = slab_block_counts[i];
		slab->alloc_count = 0;
		slab->free_count  = 0;
		mutex_init(&slab->lock);
		bitmap_zero(slab->bitmap, VPU_SLAB_MAX_BLOCKS);

		pool_bytes = slab->block_size * slab->nr_blocks;

		/*
		 * Use vzalloc for the pool — we don't need physically
		 * contiguous memory at the pool level; individual blocks
		 * are DMA-mapped on demand when exported.
		 */
		slab->pool_base = vzalloc(pool_bytes);
		if (!slab->pool_base) {
			pr_err("straylight-vpu: slab order %d: "
			       "vzalloc(%zu) failed\n", i, pool_bytes);
			goto err_rollback;
		}
		slab->pool_dma = 0; /* resolved per-block on export */

		pr_info("straylight-vpu: slab[%d] %zuB x %u = %zu MiB ready\n",
			i, slab->block_size, slab->nr_blocks,
			pool_bytes >> 20);
	}
	return 0;

err_rollback:
	while (--i >= 0) {
		vfree(vdev->slabs[i].pool_base);
		vdev->slabs[i].pool_base = NULL;
	}
	return -ENOMEM;
}

void vpu_slab_destroy(struct vpu_device *vdev)
{
	int i;

	for (i = 0; i < VPU_SLAB_ORDER_COUNT; i++) {
		struct vpu_slab *slab = &vdev->slabs[i];

		if (slab->pool_base) {
			vfree(slab->pool_base);
			slab->pool_base = NULL;
		}
		mutex_destroy(&slab->lock);
	}
}

/* ---- best-fit order selection ------------------------------------------ */

static int vpu_select_order(size_t size)
{
	int i;

	for (i = 0; i < VPU_SLAB_ORDER_COUNT; i++) {
		if (size <= vpu_slab_sizes[i])
			return i;
	}
	return -ENOSPC;
}

/* ---- allocate ---------------------------------------------------------- */

int vpu_slab_alloc(struct vpu_device *vdev, size_t size,
		   void **cpu_addr, dma_addr_t *dma_addr,
		   int *order_out, int *block_idx_out)
{
	int order;
	struct vpu_slab *slab;
	unsigned long idx;
	void *addr;

	order = vpu_select_order(size);
	if (order < 0)
		return order;

	slab = &vdev->slabs[order];

	mutex_lock(&slab->lock);

	/* Find first free block */
	idx = find_first_zero_bit(slab->bitmap, slab->nr_blocks);
	if (idx >= slab->nr_blocks) {
		/*
		 * Current order is full — try the next larger order.
		 * This provides graceful fallback at the cost of internal
		 * fragmentation.
		 */
		mutex_unlock(&slab->lock);

		if (order + 1 < VPU_SLAB_ORDER_COUNT)
			return vpu_slab_alloc(vdev, vpu_slab_sizes[order + 1],
					      cpu_addr, dma_addr,
					      order_out, block_idx_out);
		return -ENOMEM;
	}

	__set_bit(idx, slab->bitmap);
	slab->alloc_count++;
	mutex_unlock(&slab->lock);

	addr = slab->pool_base + (idx * slab->block_size);

	*cpu_addr      = addr;
	/*
	 * For vmalloc'd memory the "DMA address" is the virt_to_phys of the
	 * page backing the first page of this block.  Real hardware would
	 * use dma_map_single() here — we store the virtual-to-physical
	 * translation for mmap support.
	 */
	*dma_addr      = virt_to_phys(addr);
	*order_out     = order;
	*block_idx_out = (int)idx;

	return 0;
}

/* ---- free -------------------------------------------------------------- */

void vpu_slab_free(struct vpu_device *vdev, int order, int block_idx)
{
	struct vpu_slab *slab;

	if (order < 0 || order >= VPU_SLAB_ORDER_COUNT)
		return;

	slab = &vdev->slabs[order];

	mutex_lock(&slab->lock);

	if (block_idx < 0 || (unsigned int)block_idx >= slab->nr_blocks) {
		pr_warn("straylight-vpu: slab_free: bad block_idx %d "
			"(order %d, max %u)\n",
			block_idx, order, slab->nr_blocks);
		mutex_unlock(&slab->lock);
		return;
	}

	if (!test_bit(block_idx, slab->bitmap)) {
		pr_warn("straylight-vpu: slab_free: double free "
			"order=%d idx=%d\n", order, block_idx);
		mutex_unlock(&slab->lock);
		return;
	}

	/* Zero the freed block to prevent data leaks */
	memset(slab->pool_base + ((unsigned long)block_idx * slab->block_size),
	       0, slab->block_size);

	__clear_bit(block_idx, slab->bitmap);
	slab->free_count++;

	mutex_unlock(&slab->lock);
}
