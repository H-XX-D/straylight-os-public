// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Multi-GPU Management Implementation
 * Copyright (C) 2026 StrayLight Systems
 *
 * PCI bus discovery, per-GPU slab initialization, P2P copy via DMA
 * engine, mirrored allocations, policy-driven placement, per-GPU
 * sysfs nodes, and ioctl dispatch for the multi-GPU command set.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/thermal.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/bitmap.h>
#include <linux/string.h>
#include <linux/mutex.h>

#include "vpu_mgpu.h"

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/*
 * Find a vpu_allocation by handle across all GPUs in the context.
 * Caller must hold ctx->lock.
 */
static struct vpu_allocation *mgpu_find_alloc(struct vpu_mgpu_context *ctx,
					      __u64 handle,
					      unsigned int *gpu_idx_out)
{
	unsigned int i;
	struct vpu_allocation *alloc;

	for (i = 0; i < ctx->gpu_count; i++) {
		struct vpu_device *vdev = &ctx->gpus[i].vdev;

		mutex_lock(&vdev->alloc_lock);
		list_for_each_entry(alloc, &vdev->alloc_list, list) {
			if (alloc->handle == handle) {
				if (gpu_idx_out)
					*gpu_idx_out = i;
				mutex_unlock(&vdev->alloc_lock);
				return alloc;
			}
		}
		mutex_unlock(&vdev->alloc_lock);
	}

	return NULL;
}

/*
 * Read temperature from thermal zone associated with a PCI device.
 * Returns millidegrees Celsius, or -1000 if unavailable.
 */
static int mgpu_read_temperature(struct pci_dev *pdev)
{
	struct thermal_zone_device *tz;
	int temp = 0;

	if (!pdev || !pdev->dev.parent)
		return -1000;

	/*
	 * Attempt to find a thermal zone by name pattern.
	 * GPU drivers typically register zones like "gpu_thermal" or
	 * embed them in hwmon.  We try the PCI device's own thermal
	 * zone first, then fall back to -1.
	 */
	tz = thermal_zone_get_zone_by_name("gpu_thermal");
	if (IS_ERR(tz)) {
		/* No named zone — try device-specific lookup */
		return -1000;
	}

	if (thermal_zone_get_temp(tz, &temp))
		return -1000;

	return temp; /* millidegrees */
}

/*
 * Compute VRAM estimate from PCI BARs.
 * BAR 0 is typically MMIO, BAR 2 (or the largest BAR) is VRAM aperture.
 */
static __u64 mgpu_estimate_vram(struct pci_dev *pdev)
{
	int i;
	__u64 largest = 0;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		unsigned long flags = pci_resource_flags(pdev, i);
		__u64 len = pci_resource_len(pdev, i);

		if (!(flags & IORESOURCE_MEM))
			continue;

		/* Prefer prefetchable (VRAM) BARs */
		if ((flags & IORESOURCE_PREFETCH) && len > largest)
			largest = len;
	}

	/* Fallback: use largest memory BAR if no prefetchable found */
	if (largest == 0) {
		for (i = 0; i < PCI_STD_NUM_BARS; i++) {
			unsigned long flags = pci_resource_flags(pdev, i);
			__u64 len = pci_resource_len(pdev, i);

			if ((flags & IORESOURCE_MEM) && len > largest)
				largest = len;
		}
	}

	return largest;
}

/*
 * Get PCIe link speed in megatransfers per second.
 */
static __u32 mgpu_link_speed(struct pci_dev *pdev)
{
	u16 lnksta;
	u8 speed;

	if (pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta))
		return 0;

	speed = lnksta & PCI_EXP_LNKSTA_CLS;

	switch (speed) {
	case 1: return 2500;   /* Gen1 */
	case 2: return 5000;   /* Gen2 */
	case 3: return 8000;   /* Gen3 */
	case 4: return 16000;  /* Gen4 */
	case 5: return 32000;  /* Gen5 */
	case 6: return 64000;  /* Gen6 */
	default: return 0;
	}
}

/*
 * Get PCIe link width (number of lanes).
 */
static __u32 mgpu_link_width(struct pci_dev *pdev)
{
	u16 lnksta;

	if (pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta))
		return 0;

	return (lnksta & PCI_EXP_LNKSTA_NLW) >> 4;
}

/*
 * Check P2P DMA capability between two PCI devices.
 * Uses topology-based heuristic: devices on the same root port or
 * behind the same switch can do P2P.
 */
static bool mgpu_check_p2p(struct pci_dev *a, struct pci_dev *b)
{
	struct pci_dev *root_a, *root_b;

	if (!a || !b || a == b)
		return false;

	/*
	 * Walk up to the root port for each device.
	 * If they share the same root port, P2P is likely supported.
	 */
	root_a = pcie_find_root_port(a);
	root_b = pcie_find_root_port(b);

	if (root_a && root_b && root_a == root_b)
		return true;

	/*
	 * Different root ports — check if they are on the same bus
	 * segment (e.g., behind a PLX/Broadcom switch).
	 */
	if (a->bus == b->bus)
		return true;

	/*
	 * Conservative: different root complexes cannot P2P without
	 * explicit IOMMU support or ACS override.
	 */
	return false;
}

/*
 * Compute VRAM used for a specific GPU's slab allocator.
 */
static __u64 mgpu_vram_used(struct vpu_device *vdev)
{
	__s64 alloc = atomic64_read(&vdev->total_allocated);
	__s64 freed = atomic64_read(&vdev->total_freed);
	__s64 used = alloc - freed;

	return (used > 0) ? (__u64)used : 0;
}

/* ========================================================================
 * Discovery
 * ======================================================================== */

int vpu_mgpu_discover(struct vpu_mgpu_context *ctx)
{
	struct pci_dev *pdev = NULL;
	unsigned int count = 0;
	unsigned int i, j;

	mutex_lock(&ctx->lock);

	/*
	 * Scan for VGA-compatible controllers (class 0x0300xx)
	 * and 3D controllers (class 0x0302xx).
	 */
	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, pdev))) {
		__u16 vendor = pdev->vendor;

		if (vendor != VPU_PCI_VENDOR_NVIDIA &&
		    vendor != VPU_PCI_VENDOR_AMD &&
		    vendor != VPU_PCI_VENDOR_INTEL)
			continue;

		if (count >= VPU_MGPU_MAX_GPUS) {
			pr_warn("straylight-vpu: mgpu: max GPU count "
				"(%d) reached during VGA scan\n",
				VPU_MGPU_MAX_GPUS);
			break;
		}

		pci_dev_get(pdev); /* take reference */

		ctx->gpus[count].pdev = pdev;
		ctx->gpus[count].info.index = count;
		ctx->gpus[count].info.pci_vendor = vendor;
		ctx->gpus[count].info.pci_device = pdev->device;
		ctx->gpus[count].info.vram_total = mgpu_estimate_vram(pdev);
		ctx->gpus[count].info.vram_used = 0;
		ctx->gpus[count].info.bar0_addr =
			pci_resource_start(pdev, 0);
		ctx->gpus[count].info.bar0_size =
			pci_resource_len(pdev, 0);
		ctx->gpus[count].info.bar2_addr =
			pci_resource_start(pdev, 2);
		ctx->gpus[count].info.bar2_size =
			pci_resource_len(pdev, 2);
		ctx->gpus[count].info.link_speed = mgpu_link_speed(pdev);
		ctx->gpus[count].info.link_width = mgpu_link_width(pdev);
		ctx->gpus[count].info.temperature =
			mgpu_read_temperature(pdev);
		ctx->gpus[count].info.p2p_peers = 0;
		ctx->gpus[count].info.pad = 0;

		snprintf(ctx->gpus[count].info.pci_slot,
			 sizeof(ctx->gpus[count].info.pci_slot),
			 "%s", pci_name(pdev));

		count++;
	}

	/* Also scan for 3D controllers (compute-only GPUs) */
	pdev = NULL;
	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_3D << 8, pdev))) {
		__u16 vendor = pdev->vendor;
		bool duplicate = false;

		if (vendor != VPU_PCI_VENDOR_NVIDIA &&
		    vendor != VPU_PCI_VENDOR_AMD &&
		    vendor != VPU_PCI_VENDOR_INTEL)
			continue;

		/* Skip if already discovered in VGA scan */
		for (i = 0; i < count; i++) {
			if (ctx->gpus[i].pdev == pdev) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		if (count >= VPU_MGPU_MAX_GPUS) {
			pr_warn("straylight-vpu: mgpu: max GPU count "
				"(%d) reached during 3D scan\n",
				VPU_MGPU_MAX_GPUS);
			break;
		}

		pci_dev_get(pdev);

		ctx->gpus[count].pdev = pdev;
		ctx->gpus[count].info.index = count;
		ctx->gpus[count].info.pci_vendor = vendor;
		ctx->gpus[count].info.pci_device = pdev->device;
		ctx->gpus[count].info.vram_total = mgpu_estimate_vram(pdev);
		ctx->gpus[count].info.vram_used = 0;
		ctx->gpus[count].info.bar0_addr =
			pci_resource_start(pdev, 0);
		ctx->gpus[count].info.bar0_size =
			pci_resource_len(pdev, 0);
		ctx->gpus[count].info.bar2_addr =
			pci_resource_start(pdev, 2);
		ctx->gpus[count].info.bar2_size =
			pci_resource_len(pdev, 2);
		ctx->gpus[count].info.link_speed = mgpu_link_speed(pdev);
		ctx->gpus[count].info.link_width = mgpu_link_width(pdev);
		ctx->gpus[count].info.temperature =
			mgpu_read_temperature(pdev);
		ctx->gpus[count].info.p2p_peers = 0;
		ctx->gpus[count].info.pad = 0;

		snprintf(ctx->gpus[count].info.pci_slot,
			 sizeof(ctx->gpus[count].info.pci_slot),
			 "%s", pci_name(pdev));

		count++;
	}

	ctx->gpu_count = count;

	/* Build P2P capability matrix */
	for (i = 0; i < count; i++) {
		for (j = i + 1; j < count; j++) {
			bool capable = mgpu_check_p2p(ctx->gpus[i].pdev,
						      ctx->gpus[j].pdev);
			ctx->p2p_matrix[i][j] = capable;
			ctx->p2p_matrix[j][i] = capable;

			if (capable) {
				ctx->gpus[i].info.p2p_peers |= (1U << j);
				ctx->gpus[j].info.p2p_peers |= (1U << i);
			}
		}
	}

	mutex_unlock(&ctx->lock);

	pr_info("straylight-vpu: mgpu: discovered %u GPU(s)\n", count);
	for (i = 0; i < count; i++) {
		pr_info("straylight-vpu: mgpu: [%u] %04x:%04x at %s "
			"VRAM=%llu MiB PCIe Gen%u x%u P2P=0x%08x\n",
			i,
			ctx->gpus[i].info.pci_vendor,
			ctx->gpus[i].info.pci_device,
			ctx->gpus[i].info.pci_slot,
			ctx->gpus[i].info.vram_total >> 20,
			ctx->gpus[i].info.link_speed <= 2500  ? 1 :
			ctx->gpus[i].info.link_speed <= 5000  ? 2 :
			ctx->gpus[i].info.link_speed <= 8000  ? 3 :
			ctx->gpus[i].info.link_speed <= 16000 ? 4 :
			ctx->gpus[i].info.link_speed <= 32000 ? 5 : 6,
			ctx->gpus[i].info.link_width,
			ctx->gpus[i].info.p2p_peers);
	}

	return 0;
}

/* ========================================================================
 * Per-GPU initialization
 * ======================================================================== */

int vpu_mgpu_init_gpu(struct vpu_mgpu_context *ctx, unsigned int idx)
{
	struct vpu_mgpu_gpu *gpu;
	struct vpu_device *vdev;
	int ret;

	if (idx >= ctx->gpu_count)
		return -EINVAL;

	gpu = &ctx->gpus[idx];
	if (gpu->initialized)
		return 0;

	vdev = &gpu->vdev;

	/* Initialize the per-GPU vpu_device */
	INIT_LIST_HEAD(&vdev->alloc_list);
	mutex_init(&vdev->alloc_lock);
	atomic64_set(&vdev->total_allocated, 0);
	atomic64_set(&vdev->total_freed, 0);
	atomic64_set(&vdev->active_allocs, 0);
	vdev->dev           = &gpu->pdev->dev;
	vdev->next_handle   = (1ULL << 63) | ((__u64)idx << 48) | 1;
	vdev->next_gpu_addr = 0x100000000ULL + ((__u64)idx << 40);

	/* Initialize slab pools for this GPU */
	ret = vpu_slab_init(vdev);
	if (ret) {
		pr_err("straylight-vpu: mgpu: slab init failed for "
		       "GPU %u (%d)\n", idx, ret);
		return ret;
	}

	gpu->initialized = true;

	pr_info("straylight-vpu: mgpu: GPU %u slab pools initialized\n", idx);
	return 0;
}

/* ========================================================================
 * Teardown
 * ======================================================================== */

void vpu_mgpu_destroy(struct vpu_mgpu_context *ctx)
{
	unsigned int i;

	vpu_mgpu_sysfs_cleanup(ctx);

	for (i = 0; i < ctx->gpu_count; i++) {
		struct vpu_mgpu_gpu *gpu = &ctx->gpus[i];

		if (gpu->initialized) {
			struct vpu_allocation *alloc, *tmp;

			/* Free leaked allocations */
			mutex_lock(&gpu->vdev.alloc_lock);
			list_for_each_entry_safe(alloc, tmp,
						 &gpu->vdev.alloc_list,
						 list) {
				pr_warn("straylight-vpu: mgpu: GPU %u "
					"leaked alloc handle=%llu "
					"size=%zu\n",
					i, alloc->handle, alloc->size);
				vpu_slab_free(&gpu->vdev, alloc->order,
					      alloc->block_idx);
				list_del(&alloc->list);
				kfree(alloc);
			}
			mutex_unlock(&gpu->vdev.alloc_lock);

			vpu_slab_destroy(&gpu->vdev);
			gpu->initialized = false;
		}

		if (gpu->pdev) {
			pci_dev_put(gpu->pdev);
			gpu->pdev = NULL;
		}
	}

	ctx->gpu_count = 0;
}

/* ========================================================================
 * Allocation on specific GPU
 * ======================================================================== */

int vpu_mgpu_alloc_on(struct vpu_mgpu_context *ctx, unsigned int gpu_idx,
		      __u64 size, __u32 flags,
		      struct vpu_allocation **alloc_out)
{
	struct vpu_mgpu_gpu *gpu;
	struct vpu_device *vdev;
	struct vpu_allocation *alloc;
	void *cpu_addr;
	dma_addr_t dma_addr;
	int order, block_idx, ret;

	if (gpu_idx >= ctx->gpu_count)
		return -EINVAL;

	gpu = &ctx->gpus[gpu_idx];
	if (!gpu->initialized) {
		ret = vpu_mgpu_init_gpu(ctx, gpu_idx);
		if (ret)
			return ret;
	}

	vdev = &gpu->vdev;

	if (size == 0)
		return -EINVAL;

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return -ENOMEM;

	ret = vpu_slab_alloc(vdev, (size_t)size, &cpu_addr, &dma_addr,
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
	alloc->flags     = flags;
	alloc->vdev      = vdev;
	alloc->dmabuf    = NULL;
	alloc->gpu_addr  = vdev->next_gpu_addr;
	vdev->next_gpu_addr += alloc->size;

	list_add_tail(&alloc->list, &vdev->alloc_list);
	atomic64_add(alloc->size, &vdev->total_allocated);
	atomic64_inc(&vdev->active_allocs);

	mutex_unlock(&vdev->alloc_lock);

	/* Update cached VRAM usage */
	gpu->info.vram_used = mgpu_vram_used(vdev);

	*alloc_out = alloc;
	return 0;
}

/* ========================================================================
 * Peer-to-peer copy
 * ======================================================================== */

int vpu_mgpu_p2p_copy(struct vpu_mgpu_context *ctx,
		      __u64 src_handle, __u64 dst_handle,
		      __u64 size, __u64 offset)
{
	struct vpu_allocation *src_alloc, *dst_alloc;
	unsigned int src_gpu, dst_gpu;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	enum dma_status status;
	dma_addr_t src_dma, dst_dma;

	mutex_lock(&ctx->lock);

	src_alloc = mgpu_find_alloc(ctx, src_handle, &src_gpu);
	if (!src_alloc) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	dst_alloc = mgpu_find_alloc(ctx, dst_handle, &dst_gpu);
	if (!dst_alloc) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	/* Validate copy bounds */
	if (offset + size > src_alloc->size ||
	    offset + size > dst_alloc->size) {
		mutex_unlock(&ctx->lock);
		return -EINVAL;
	}

	/* Check P2P capability if cross-GPU */
	if (src_gpu != dst_gpu && !ctx->p2p_matrix[src_gpu][dst_gpu]) {
		pr_warn("straylight-vpu: mgpu: P2P not available "
			"between GPU %u and GPU %u, using CPU bounce\n",
			src_gpu, dst_gpu);
		/*
		 * Fall back to CPU-mediated copy via kernel virtual
		 * addresses (both are vmalloc'd slab blocks).
		 */
		memcpy((char *)dst_alloc->cpu_addr + offset,
		       (char *)src_alloc->cpu_addr + offset,
		       (size_t)size);
		mutex_unlock(&ctx->lock);
		return 0;
	}

	/* Same GPU or P2P capable: attempt DMA engine transfer */
	src_dma = src_alloc->dma_addr + offset;
	dst_dma = dst_alloc->dma_addr + offset;

	mutex_unlock(&ctx->lock);

	/* Request a DMA channel for memory-to-memory copy */
	chan = dma_request_chan(ctx->gpus[src_gpu].vdev.dev, "memcpy");
	if (IS_ERR(chan)) {
		/*
		 * No DMA engine channel available — fall back to
		 * CPU memcpy through kernel virtual addresses.
		 */
		memcpy((char *)dst_alloc->cpu_addr + offset,
		       (char *)src_alloc->cpu_addr + offset,
		       (size_t)size);
		return 0;
	}

	tx = dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma,
				       (size_t)size, DMA_PREP_INTERRUPT);
	if (!tx) {
		pr_warn("straylight-vpu: mgpu: DMA prep failed, "
			"falling back to CPU copy\n");
		dma_release_channel(chan);
		memcpy((char *)dst_alloc->cpu_addr + offset,
		       (char *)src_alloc->cpu_addr + offset,
		       (size_t)size);
		return 0;
	}

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		pr_warn("straylight-vpu: mgpu: DMA submit failed, "
			"falling back to CPU copy\n");
		dma_release_channel(chan);
		memcpy((char *)dst_alloc->cpu_addr + offset,
		       (char *)src_alloc->cpu_addr + offset,
		       (size_t)size);
		return 0;
	}

	dma_async_issue_pending(chan);

	/* Wait for completion with 5 second timeout */
	status = dma_sync_wait(chan, cookie);
	dma_release_channel(chan);

	if (status != DMA_COMPLETE) {
		pr_err("straylight-vpu: mgpu: DMA transfer failed "
		       "(status=%d), falling back to CPU copy\n", status);
		memcpy((char *)dst_alloc->cpu_addr + offset,
		       (char *)src_alloc->cpu_addr + offset,
		       (size_t)size);
	}

	return 0;
}

/* ========================================================================
 * Mirrored allocation
 * ======================================================================== */

int vpu_mgpu_mirror(struct vpu_mgpu_context *ctx,
		    __u64 src_handle, __u32 gpu_mask,
		    __u64 *mirror_handles, __u32 *count_out)
{
	struct vpu_allocation *src_alloc;
	unsigned int src_gpu;
	unsigned int i;
	__u32 count = 0;
	int ret;

	mutex_lock(&ctx->lock);

	src_alloc = mgpu_find_alloc(ctx, src_handle, &src_gpu);
	if (!src_alloc) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	mutex_unlock(&ctx->lock);

	/* Create mirrored allocations on each target GPU */
	for (i = 0; i < ctx->gpu_count && i < 32; i++) {
		struct vpu_allocation *mirror_alloc;

		if (!(gpu_mask & (1U << i)))
			continue;

		/* Skip the source GPU */
		if (i == src_gpu)
			continue;

		ret = vpu_mgpu_alloc_on(ctx, i, src_alloc->size,
					src_alloc->flags, &mirror_alloc);
		if (ret) {
			pr_warn("straylight-vpu: mgpu: mirror alloc "
				"on GPU %u failed (%d)\n", i, ret);
			continue;
		}

		/* Copy data from source to mirror */
		memcpy(mirror_alloc->cpu_addr, src_alloc->cpu_addr,
		       src_alloc->size);

		mirror_handles[count] = mirror_alloc->handle;
		count++;
	}

	*count_out = count;

	if (count == 0)
		return -ENOMEM;

	pr_info("straylight-vpu: mgpu: mirrored handle %llu to %u GPU(s)\n",
		src_handle, count);

	return 0;
}

/* ========================================================================
 * Policy-driven balanced allocation
 * ======================================================================== */

int vpu_mgpu_balance_alloc(struct vpu_mgpu_context *ctx,
			   __u64 size, __u32 flags, __u32 policy,
			   struct vpu_allocation **alloc_out)
{
	unsigned int target;
	unsigned int i;
	__u64 min_used;

	if (ctx->gpu_count == 0)
		return -ENODEV;

	switch (policy) {
	case VPU_POLICY_ROUND_ROBIN:
		mutex_lock(&ctx->lock);
		target = ctx->rr_counter % ctx->gpu_count;
		ctx->rr_counter++;
		mutex_unlock(&ctx->lock);
		break;

	case VPU_POLICY_LEAST_USED:
		target = 0;
		min_used = ULLONG_MAX;
		for (i = 0; i < ctx->gpu_count; i++) {
			__u64 used = mgpu_vram_used(&ctx->gpus[i].vdev);

			if (used < min_used) {
				min_used = used;
				target = i;
			}
		}
		break;

	case VPU_POLICY_AFFINITY:
		/*
		 * Affinity: stick to the GPU that was last used
		 * (approximated by rr_counter - 1 mod gpu_count).
		 */
		mutex_lock(&ctx->lock);
		if (ctx->rr_counter > 0)
			target = (ctx->rr_counter - 1) % ctx->gpu_count;
		else
			target = 0;
		mutex_unlock(&ctx->lock);
		break;

	default:
		return -EINVAL;
	}

	return vpu_mgpu_alloc_on(ctx, target, size, flags, alloc_out);
}

int vpu_mgpu_free_handle(struct vpu_mgpu_context *ctx, __u64 handle)
{
	unsigned int i;

	mutex_lock(&ctx->lock);
	for (i = 0; i < ctx->gpu_count; i++) {
		struct vpu_device *vdev = &ctx->gpus[i].vdev;
		struct vpu_allocation *alloc, *tmp;

		if (!ctx->gpus[i].initialized)
			continue;

		mutex_lock(&vdev->alloc_lock);
		list_for_each_entry_safe(alloc, tmp, &vdev->alloc_list, list) {
			if (alloc->handle != handle)
				continue;

			if (alloc->dmabuf) {
				mutex_unlock(&vdev->alloc_lock);
				mutex_unlock(&ctx->lock);
				return -EBUSY;
			}

			list_del(&alloc->list);
			mutex_unlock(&vdev->alloc_lock);

			if (alloc->order >= 0)
				vpu_slab_free(vdev, alloc->order,
					      alloc->block_idx);
			atomic64_add(alloc->size, &vdev->total_freed);
			atomic64_dec(&vdev->active_allocs);
			ctx->gpus[i].info.vram_used = mgpu_vram_used(vdev);
			kfree(alloc);
			mutex_unlock(&ctx->lock);
			return 0;
		}
		mutex_unlock(&vdev->alloc_lock);
	}
	mutex_unlock(&ctx->lock);

	return -ENOENT;
}

int vpu_mgpu_query_handle(struct vpu_mgpu_context *ctx, __u64 handle,
			  struct vpu_query_req *req)
{
	struct vpu_allocation *alloc;

	mutex_lock(&ctx->lock);
	alloc = mgpu_find_alloc(ctx, handle, NULL);
	if (!alloc) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	req->handle   = handle;
	req->size     = alloc->size;
	req->order    = alloc->order;
	req->flags    = alloc->flags;
	req->gpu_addr = alloc->gpu_addr;

	mutex_unlock(&ctx->lock);
	return 0;
}

int vpu_mgpu_export_dma(struct vpu_mgpu_context *ctx, __u64 handle,
			int flags, int *fd_out)
{
	struct vpu_allocation *alloc;
	int ret;

	mutex_lock(&ctx->lock);
	alloc = mgpu_find_alloc(ctx, handle, NULL);
	if (!alloc) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	ret = vpu_dma_buf_export(alloc, flags, fd_out);
	mutex_unlock(&ctx->lock);
	return ret;
}

/* ========================================================================
 * sysfs — per-GPU nodes
 * ======================================================================== */

/*
 * Each GPU gets a directory under /sys/kernel/straylight-vpu/gpu{N}/
 * with attributes: vendor, device, vram_total, vram_used, temperature,
 * p2p_peers, link_speed.
 */

struct mgpu_sysfs_ctx {
	struct vpu_mgpu_context *ctx;
	unsigned int gpu_idx;
};

static struct mgpu_sysfs_ctx sysfs_gpu_ctx[VPU_MGPU_MAX_GPUS];

#define MGPU_SYSFS_ATTR_SHOW(_name, _fmt, _field)                         \
static ssize_t gpu_##_name##_show(struct kobject *kobj,                    \
				  struct kobj_attribute *attr,              \
				  char *buf)                               \
{                                                                          \
	unsigned int i;                                                    \
	struct vpu_mgpu_context *c = NULL;                                 \
	for (i = 0; i < VPU_MGPU_MAX_GPUS; i++) {                         \
		if (sysfs_gpu_ctx[i].ctx &&                                \
		    sysfs_gpu_ctx[i].ctx->gpus[sysfs_gpu_ctx[i].gpu_idx]   \
			.sysfs_kobj == kobj) {                             \
			c = sysfs_gpu_ctx[i].ctx;                          \
			break;                                             \
		}                                                          \
	}                                                                  \
	if (!c)                                                            \
		return -ENODEV;                                            \
	return sysfs_emit(buf, _fmt "\n",                                  \
			  c->gpus[sysfs_gpu_ctx[i].gpu_idx].info._field);  \
}

MGPU_SYSFS_ATTR_SHOW(vendor,      "0x%04x", pci_vendor)
MGPU_SYSFS_ATTR_SHOW(device,      "0x%04x", pci_device)
MGPU_SYSFS_ATTR_SHOW(vram_total,  "%llu",   vram_total)
MGPU_SYSFS_ATTR_SHOW(p2p_peers,   "0x%08x", p2p_peers)
MGPU_SYSFS_ATTR_SHOW(link_speed,  "%u",     link_speed)

static ssize_t gpu_vram_used_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	unsigned int i;
	struct vpu_mgpu_context *c = NULL;

	for (i = 0; i < VPU_MGPU_MAX_GPUS; i++) {
		if (sysfs_gpu_ctx[i].ctx &&
		    sysfs_gpu_ctx[i].ctx->gpus[sysfs_gpu_ctx[i].gpu_idx]
			.sysfs_kobj == kobj) {
			c = sysfs_gpu_ctx[i].ctx;
			break;
		}
	}
	if (!c)
		return -ENODEV;

	return sysfs_emit(buf, "%llu\n",
		mgpu_vram_used(&c->gpus[sysfs_gpu_ctx[i].gpu_idx].vdev));
}

static ssize_t gpu_temperature_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	unsigned int i;
	struct vpu_mgpu_context *c = NULL;
	int temp;

	for (i = 0; i < VPU_MGPU_MAX_GPUS; i++) {
		if (sysfs_gpu_ctx[i].ctx &&
		    sysfs_gpu_ctx[i].ctx->gpus[sysfs_gpu_ctx[i].gpu_idx]
			.sysfs_kobj == kobj) {
			c = sysfs_gpu_ctx[i].ctx;
			break;
		}
	}
	if (!c)
		return -ENODEV;

	/* Re-read temperature dynamically */
	temp = mgpu_read_temperature(
		c->gpus[sysfs_gpu_ctx[i].gpu_idx].pdev);
	c->gpus[sysfs_gpu_ctx[i].gpu_idx].info.temperature = temp;

	return sysfs_emit(buf, "%d\n", temp);
}

static struct kobj_attribute attr_gpu_vendor =
	__ATTR(vendor,      0444, gpu_vendor_show,      NULL);
static struct kobj_attribute attr_gpu_device =
	__ATTR(device,      0444, gpu_device_show,      NULL);
static struct kobj_attribute attr_gpu_vram_total =
	__ATTR(vram_total,  0444, gpu_vram_total_show,  NULL);
static struct kobj_attribute attr_gpu_vram_used =
	__ATTR(vram_used,   0444, gpu_vram_used_show,   NULL);
static struct kobj_attribute attr_gpu_temperature =
	__ATTR(temperature, 0444, gpu_temperature_show, NULL);
static struct kobj_attribute attr_gpu_p2p_peers =
	__ATTR(p2p_peers,   0444, gpu_p2p_peers_show,   NULL);
static struct kobj_attribute attr_gpu_link_speed =
	__ATTR(link_speed,  0444, gpu_link_speed_show,  NULL);

static struct attribute *mgpu_gpu_attrs[] = {
	&attr_gpu_vendor.attr,
	&attr_gpu_device.attr,
	&attr_gpu_vram_total.attr,
	&attr_gpu_vram_used.attr,
	&attr_gpu_temperature.attr,
	&attr_gpu_p2p_peers.attr,
	&attr_gpu_link_speed.attr,
	NULL,
};

static const struct attribute_group mgpu_gpu_attr_group = {
	.attrs = mgpu_gpu_attrs,
};

int vpu_mgpu_sysfs_init(struct vpu_mgpu_context *ctx,
			struct kobject *parent)
{
	unsigned int i;
	char name[16];

	ctx->parent_kobj = parent;

	for (i = 0; i < ctx->gpu_count; i++) {
		int ret;

		snprintf(name, sizeof(name), "gpu%u", i);

		ctx->gpus[i].sysfs_kobj =
			kobject_create_and_add(name, parent);
		if (!ctx->gpus[i].sysfs_kobj) {
			pr_warn("straylight-vpu: mgpu: sysfs: "
				"failed to create %s\n", name);
			continue;
		}

		sysfs_gpu_ctx[i].ctx = ctx;
		sysfs_gpu_ctx[i].gpu_idx = i;

		ret = sysfs_create_group(ctx->gpus[i].sysfs_kobj,
					 &mgpu_gpu_attr_group);
		if (ret) {
			pr_warn("straylight-vpu: mgpu: sysfs: "
				"failed to create attrs for %s (%d)\n",
				name, ret);
			kobject_put(ctx->gpus[i].sysfs_kobj);
			ctx->gpus[i].sysfs_kobj = NULL;
		}
	}

	return 0;
}

void vpu_mgpu_sysfs_cleanup(struct vpu_mgpu_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->gpu_count; i++) {
		if (ctx->gpus[i].sysfs_kobj) {
			sysfs_remove_group(ctx->gpus[i].sysfs_kobj,
					   &mgpu_gpu_attr_group);
			kobject_put(ctx->gpus[i].sysfs_kobj);
			ctx->gpus[i].sysfs_kobj = NULL;
		}
		sysfs_gpu_ctx[i].ctx = NULL;
	}
}

/* ========================================================================
 * ioctl dispatch for multi-GPU commands
 * ======================================================================== */

static long mgpu_ioctl_enum_gpus(struct vpu_mgpu_context *ctx,
				 unsigned long arg)
{
	struct vpu_enum_gpus_req req;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.count = ctx->gpu_count;

	for (i = 0; i < ctx->gpu_count; i++) {
		/* Refresh vram_used */
		ctx->gpus[i].info.vram_used =
			mgpu_vram_used(&ctx->gpus[i].vdev);
		/* Refresh temperature */
		ctx->gpus[i].info.temperature =
			mgpu_read_temperature(ctx->gpus[i].pdev);

		memcpy(&req.gpus[i], &ctx->gpus[i].info,
		       sizeof(struct vpu_gpu_info));
	}

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long mgpu_ioctl_alloc_on(struct vpu_mgpu_context *ctx,
				unsigned long arg)
{
	struct vpu_alloc_on_req req;
	struct vpu_allocation *alloc;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	ret = vpu_mgpu_alloc_on(ctx, req.gpu_index, req.size, req.flags,
				&alloc);
	if (ret)
		return ret;

	req.handle   = alloc->handle;
	req.gpu_addr = alloc->gpu_addr;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long mgpu_ioctl_p2p_copy(struct vpu_mgpu_context *ctx,
				unsigned long arg)
{
	struct vpu_p2p_copy_req req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	return vpu_mgpu_p2p_copy(ctx, req.src_handle, req.dst_handle,
				 req.size, req.offset);
}

static long mgpu_ioctl_mirror(struct vpu_mgpu_context *ctx,
			      unsigned long arg)
{
	struct vpu_mirror_req req;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	ret = vpu_mgpu_mirror(ctx, req.src_handle, req.gpu_mask,
			      req.mirror_handles, &req.count);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long mgpu_ioctl_gpu_stats(struct vpu_mgpu_context *ctx,
				 unsigned long arg)
{
	struct vpu_gpu_stats_req req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.gpu_index >= ctx->gpu_count)
		return -EINVAL;

	/* Refresh dynamic fields */
	ctx->gpus[req.gpu_index].info.vram_used =
		mgpu_vram_used(&ctx->gpus[req.gpu_index].vdev);
	ctx->gpus[req.gpu_index].info.temperature =
		mgpu_read_temperature(ctx->gpus[req.gpu_index].pdev);

	memcpy(&req.info, &ctx->gpus[req.gpu_index].info,
	       sizeof(struct vpu_gpu_info));

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long vpu_mgpu_ioctl(struct vpu_mgpu_context *ctx, unsigned int cmd,
		    unsigned long arg)
{
	switch (cmd) {
	case VPU_IOC_ENUM_GPUS:
		return mgpu_ioctl_enum_gpus(ctx, arg);
	case VPU_IOC_ALLOC_ON:
		return mgpu_ioctl_alloc_on(ctx, arg);
	case VPU_IOC_P2P_COPY:
		return mgpu_ioctl_p2p_copy(ctx, arg);
	case VPU_IOC_MIRROR:
		return mgpu_ioctl_mirror(ctx, arg);
	case VPU_IOC_GPU_STATS:
		return mgpu_ioctl_gpu_stats(ctx, arg);
	default:
		return -ENOTTY;
	}
}
