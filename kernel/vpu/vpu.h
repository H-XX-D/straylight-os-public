/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — GPU Memory Slab Allocator
 * Copyright (C) 2026 StrayLight Systems
 *
 * VPU device context, ioctl structures, and slab allocator interface.
 */

#ifndef _STRAYLIGHT_VPU_H
#define _STRAYLIGHT_VPU_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-buf.h>
#include <linux/kobject.h>
#include <linux/bitmap.h>

/* ---------- slab orders ------------------------------------------------- */
#define VPU_SLAB_ORDER_COUNT    5

#define VPU_SLAB_ORDER_4K       0       /* 4 KiB   */
#define VPU_SLAB_ORDER_64K      1       /* 64 KiB  */
#define VPU_SLAB_ORDER_1M       2       /* 1 MiB   */
#define VPU_SLAB_ORDER_16M      3       /* 16 MiB  */
#define VPU_SLAB_ORDER_256M     4       /* 256 MiB */

static const size_t vpu_slab_sizes[VPU_SLAB_ORDER_COUNT] = {
	4UL   * 1024,
	64UL  * 1024,
	1UL   * 1024 * 1024,
	16UL  * 1024 * 1024,
	256UL * 1024 * 1024,
};

/* Maximum number of blocks per slab order */
#define VPU_SLAB_MAX_BLOCKS     1024

/* ---------- ioctl numbers ----------------------------------------------- */
#define VPU_IOC_MAGIC           'V'

#define VPU_IOC_ALLOC           _IOWR(VPU_IOC_MAGIC, 0x01, struct vpu_alloc_req)
#define VPU_IOC_FREE            _IOW (VPU_IOC_MAGIC, 0x02, struct vpu_free_req)
#define VPU_IOC_MAP             _IOWR(VPU_IOC_MAGIC, 0x03, struct vpu_map_req)
#define VPU_IOC_QUERY           _IOWR(VPU_IOC_MAGIC, 0x04, struct vpu_query_req)
#define VPU_IOC_EXPORT_DMA      _IOWR(VPU_IOC_MAGIC, 0x05, struct vpu_dma_export_req)
#define VPU_IOC_IMPORT_DMA      _IOWR(VPU_IOC_MAGIC, 0x06, struct vpu_dma_import_req)

/* ---------- ioctl request structs --------------------------------------- */
struct vpu_alloc_req {
	__u64 size;             /* in: requested size in bytes                */
	__u32 flags;            /* in: allocation flags                       */
	__u32 pad;
	__u64 handle;           /* out: opaque allocation handle              */
	__u64 gpu_addr;         /* out: GPU-side virtual address              */
};

struct vpu_free_req {
	__u64 handle;           /* in: handle returned by ALLOC               */
};

struct vpu_map_req {
	__u64 handle;           /* in: handle returned by ALLOC               */
	__u64 offset;           /* out: mmap offset for userspace             */
	__u64 size;             /* out: mapping size                          */
};

struct vpu_query_req {
	__u64 handle;           /* in: handle returned by ALLOC               */
	__u64 size;             /* out: allocation size                       */
	__u32 order;            /* out: slab order index                      */
	__u32 flags;            /* out: allocation flags                      */
	__u64 gpu_addr;         /* out: GPU virtual address                   */
};

struct vpu_dma_export_req {
	__u64 handle;           /* in: allocation handle                      */
	__s32 fd;               /* out: dma-buf file descriptor               */
	__u32 flags;            /* in: export flags (O_CLOEXEC etc.)          */
};

struct vpu_dma_import_req {
	__s32 fd;               /* in: dma-buf fd to import                   */
	__u32 flags;            /* in: import flags                           */
	__u64 handle;           /* out: local allocation handle               */
};

/* Allocation flags */
#define VPU_ALLOC_FLAG_COHERENT         (1U << 0)
#define VPU_ALLOC_FLAG_UNCACHED         (1U << 1)
#define VPU_ALLOC_FLAG_CONTIGUOUS       (1U << 2)

/* ---------- internal allocation tracking -------------------------------- */
struct vpu_allocation {
	struct list_head        list;
	__u64                   handle;
	int                     order;          /* slab order index       */
	int                     block_idx;      /* index within slab      */
	size_t                  size;           /* actual allocation size  */
	void                    *cpu_addr;      /* kernel virtual address  */
	dma_addr_t              dma_addr;       /* physical / DMA address  */
	__u64                   gpu_addr;       /* GPU-mapped VA           */
	__u32                   flags;
	struct dma_buf          *dmabuf;        /* exported dma-buf or NULL */
	struct vpu_device       *vdev;
};

/* ---------- per-order slab ---------------------------------------------- */
struct vpu_slab {
	size_t                  block_size;
	unsigned int            nr_blocks;
	DECLARE_BITMAP(bitmap, VPU_SLAB_MAX_BLOCKS);
	struct mutex            lock;
	void                    *pool_base;     /* vmalloc'd region       */
	dma_addr_t              pool_dma;
	unsigned long           alloc_count;
	unsigned long           free_count;
};

/* ---------- device context ---------------------------------------------- */
struct vpu_device {
	struct miscdevice       miscdev;
	struct device           *dev;
	struct kobject          *sysfs_kobj;

	struct vpu_slab         slabs[VPU_SLAB_ORDER_COUNT];

	struct list_head        alloc_list;
	struct mutex            alloc_lock;
	__u64                   next_handle;
	__u64                   next_gpu_addr;

	/* stats */
	atomic64_t              total_allocated;
	atomic64_t              total_freed;
	atomic64_t              active_allocs;
};

/* ---------- sub-module API ---------------------------------------------- */

/* vpu_slab.c */
int  vpu_slab_init(struct vpu_device *vdev);
void vpu_slab_destroy(struct vpu_device *vdev);
int  vpu_slab_alloc(struct vpu_device *vdev, size_t size,
		    void **cpu_addr, dma_addr_t *dma_addr, int *order_out,
		    int *block_idx_out);
void vpu_slab_free(struct vpu_device *vdev, int order, int block_idx);

/* vpu_ioctl.c */
long vpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* vpu_dma.c */
int  vpu_dma_buf_export(struct vpu_allocation *alloc, int flags, int *fd_out);
int  vpu_dma_buf_import(struct vpu_device *vdev, int fd, int flags,
			struct vpu_allocation **alloc_out);

/* vpu_sysfs.c */
int  vpu_sysfs_init(struct vpu_device *vdev);
void vpu_sysfs_cleanup(struct vpu_device *vdev);

#endif /* _STRAYLIGHT_VPU_H */
