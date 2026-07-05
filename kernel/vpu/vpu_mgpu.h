/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — Multi-GPU Management Layer
 * Copyright (C) 2026 StrayLight Systems
 *
 * Structures and API for discovering, enumerating, and managing
 * multiple GPUs behind the VPU slab allocator.  Extends the VPU
 * ioctl namespace with GPU-aware allocation, peer-to-peer copies,
 * and mirrored allocations.
 */

#ifndef _STRAYLIGHT_VPU_MGPU_H
#define _STRAYLIGHT_VPU_MGPU_H

#include "vpu.h"

#include <linux/pci.h>
#include <linux/thermal.h>
#include <linux/dmaengine.h>

/* ---------- limits ------------------------------------------------------ */
#define VPU_MGPU_MAX_GPUS       16

/* ---------- vendor IDs -------------------------------------------------- */
#define VPU_PCI_VENDOR_NVIDIA   0x10de
#define VPU_PCI_VENDOR_AMD      0x1002
#define VPU_PCI_VENDOR_INTEL    0x8086

/* ---------- PCI class codes --------------------------------------------- */
#define VPU_PCI_CLASS_VGA       0x030000
#define VPU_PCI_CLASS_3D        0x030200

/* ---------- allocation placement policies ------------------------------- */
#define VPU_POLICY_ROUND_ROBIN  0
#define VPU_POLICY_LEAST_USED   1
#define VPU_POLICY_AFFINITY     2

/* ---------- per-GPU information (exposed to userspace) ------------------- */
struct vpu_gpu_info {
	__u32 index;
	__u16 pci_vendor;
	__u16 pci_device;
	__u64 vram_total;       /* bytes — estimated from BARs             */
	__u64 vram_used;        /* bytes — tracked by slab allocator       */
	__u64 bar0_addr;        /* BAR 0 physical base                     */
	__u64 bar0_size;        /* BAR 0 size                              */
	__u64 bar2_addr;        /* BAR 2 physical base (VRAM aperture)     */
	__u64 bar2_size;        /* BAR 2 size                              */
	__u32 link_speed;       /* PCIe link speed in MT/s                 */
	__u32 link_width;       /* PCIe link width (lanes)                 */
	__s32 temperature;      /* millidegrees Celsius, -1 if unavailable */
	__u32 p2p_peers;        /* bitmask of P2P-capable peer GPU indices */
	char  pci_slot[16];     /* "0000:01:00.0"                          */
	__u32 pad;
};

/* ---------- ioctl request structures ------------------------------------ */

struct vpu_enum_gpus_req {
	__u32 count;                            /* out: number of GPUs     */
	__u32 pad;
	struct vpu_gpu_info gpus[VPU_MGPU_MAX_GPUS]; /* out: GPU info array */
};

struct vpu_alloc_on_req {
	__u32 gpu_index;        /* in: target GPU index                    */
	__u32 flags;            /* in: allocation flags                    */
	__u64 size;             /* in: requested size in bytes             */
	__u64 handle;           /* out: opaque allocation handle           */
	__u64 gpu_addr;         /* out: GPU-side virtual address           */
};

struct vpu_p2p_copy_req {
	__u64 src_handle;       /* in: source allocation handle            */
	__u64 dst_handle;       /* in: destination allocation handle       */
	__u64 size;             /* in: bytes to copy                       */
	__u64 offset;           /* in: byte offset within allocations      */
};

struct vpu_mirror_req {
	__u64 src_handle;       /* in: source allocation handle            */
	__u32 gpu_mask;         /* in: bitmask of target GPUs              */
	__u32 count;            /* out: number of mirrors created          */
	__u64 mirror_handles[VPU_MGPU_MAX_GPUS]; /* out: per-GPU handles  */
};

struct vpu_gpu_stats_req {
	__u32 gpu_index;        /* in: which GPU                           */
	__u32 pad;
	struct vpu_gpu_info info; /* out: current stats                    */
};

/* ---------- ioctl numbers (VPU_IOC_MAGIC 'V', starting at 0x10) --------- */

#define VPU_IOC_ENUM_GPUS   _IOR (VPU_IOC_MAGIC, 0x10, struct vpu_enum_gpus_req)
#define VPU_IOC_ALLOC_ON    _IOWR(VPU_IOC_MAGIC, 0x11, struct vpu_alloc_on_req)
#define VPU_IOC_P2P_COPY    _IOW (VPU_IOC_MAGIC, 0x12, struct vpu_p2p_copy_req)
#define VPU_IOC_MIRROR      _IOWR(VPU_IOC_MAGIC, 0x13, struct vpu_mirror_req)
#define VPU_IOC_GPU_STATS   _IOWR(VPU_IOC_MAGIC, 0x14, struct vpu_gpu_stats_req)

/* ---------- multi-GPU context ------------------------------------------- */

struct vpu_mgpu_gpu {
	struct vpu_device       vdev;           /* per-GPU slab device     */
	struct vpu_gpu_info     info;           /* cached discovery info   */
	struct pci_dev          *pdev;          /* PCI device backpointer  */
	struct kobject          *sysfs_kobj;    /* /sys/.../gpu{N}/        */
	bool                    initialized;
};

struct vpu_mgpu_context {
	struct vpu_mgpu_gpu     gpus[VPU_MGPU_MAX_GPUS];
	unsigned int            gpu_count;
	bool                    p2p_matrix[VPU_MGPU_MAX_GPUS][VPU_MGPU_MAX_GPUS];
	struct kobject          *parent_kobj;   /* straylight-vpu kobj     */
	struct mutex            lock;
	unsigned int            rr_counter;     /* round-robin state       */
};

/* ---------- sub-module API ---------------------------------------------- */

/* vpu_mgpu.c */
int  vpu_mgpu_discover(struct vpu_mgpu_context *ctx);
int  vpu_mgpu_init_gpu(struct vpu_mgpu_context *ctx, unsigned int idx);
void vpu_mgpu_destroy(struct vpu_mgpu_context *ctx);

int  vpu_mgpu_alloc_on(struct vpu_mgpu_context *ctx, unsigned int gpu_idx,
		       __u64 size, __u32 flags,
		       struct vpu_allocation **alloc_out);
int  vpu_mgpu_p2p_copy(struct vpu_mgpu_context *ctx,
		       __u64 src_handle, __u64 dst_handle,
		       __u64 size, __u64 offset);
int  vpu_mgpu_mirror(struct vpu_mgpu_context *ctx,
		     __u64 src_handle, __u32 gpu_mask,
		     __u64 *mirror_handles, __u32 *count_out);
int  vpu_mgpu_balance_alloc(struct vpu_mgpu_context *ctx,
			    __u64 size, __u32 flags, __u32 policy,
			    struct vpu_allocation **alloc_out);
int  vpu_mgpu_free_handle(struct vpu_mgpu_context *ctx, __u64 handle);
int  vpu_mgpu_query_handle(struct vpu_mgpu_context *ctx, __u64 handle,
			   struct vpu_query_req *req);
int  vpu_mgpu_export_dma(struct vpu_mgpu_context *ctx, __u64 handle,
			 int flags, int *fd_out);

/* ioctl dispatcher for multi-GPU commands */
long vpu_mgpu_ioctl(struct vpu_mgpu_context *ctx, unsigned int cmd,
		    unsigned long arg);

/* sysfs for per-GPU nodes */
int  vpu_mgpu_sysfs_init(struct vpu_mgpu_context *ctx,
			 struct kobject *parent);
void vpu_mgpu_sysfs_cleanup(struct vpu_mgpu_context *ctx);

#endif /* _STRAYLIGHT_VPU_MGPU_H */
