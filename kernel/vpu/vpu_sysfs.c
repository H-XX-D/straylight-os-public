// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VPU sysfs Interface
 * Copyright (C) 2026 StrayLight Systems
 *
 * Exposes allocation statistics and per-slab usage under
 * /sys/kernel/straylight-vpu/
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/bitmap.h>

#include "vpu.h"

static struct vpu_device *sysfs_vdev;

/* ---- top-level attributes --------------------------------------------- */

static ssize_t total_allocated_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&sysfs_vdev->total_allocated));
}

static ssize_t total_freed_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&sysfs_vdev->total_freed));
}

static ssize_t active_allocs_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&sysfs_vdev->active_allocs));
}

static ssize_t slab_usage_show(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	int i;
	int len = 0;

	for (i = 0; i < VPU_SLAB_ORDER_COUNT; i++) {
		struct vpu_slab *slab = &sysfs_vdev->slabs[i];
		unsigned int used;

		mutex_lock(&slab->lock);
		used = bitmap_weight(slab->bitmap, slab->nr_blocks);
		mutex_unlock(&slab->lock);

		len += sysfs_emit_at(buf, len,
			"order[%d] block_size=%-10zu  used=%u/%u  "
			"allocs=%lu  frees=%lu\n",
			i, slab->block_size, used, slab->nr_blocks,
			slab->alloc_count, slab->free_count);
	}

	return len;
}

static ssize_t memory_pressure_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	int i;
	unsigned long total_blocks = 0;
	unsigned long used_blocks  = 0;
	unsigned int pressure_pct;

	for (i = 0; i < VPU_SLAB_ORDER_COUNT; i++) {
		struct vpu_slab *slab = &sysfs_vdev->slabs[i];
		unsigned int used;

		mutex_lock(&slab->lock);
		used = bitmap_weight(slab->bitmap, slab->nr_blocks);
		mutex_unlock(&slab->lock);

		total_blocks += slab->nr_blocks;
		used_blocks  += used;
	}

	if (total_blocks == 0)
		pressure_pct = 0;
	else
		pressure_pct = (unsigned int)((used_blocks * 100) / total_blocks);

	return sysfs_emit(buf, "%u\n", pressure_pct);
}

static struct kobj_attribute attr_total_allocated =
	__ATTR_RO(total_allocated);
static struct kobj_attribute attr_total_freed =
	__ATTR_RO(total_freed);
static struct kobj_attribute attr_active_allocs =
	__ATTR_RO(active_allocs);
static struct kobj_attribute attr_slab_usage =
	__ATTR_RO(slab_usage);
static struct kobj_attribute attr_memory_pressure =
	__ATTR_RO(memory_pressure);

static struct attribute *vpu_attrs[] = {
	&attr_total_allocated.attr,
	&attr_total_freed.attr,
	&attr_active_allocs.attr,
	&attr_slab_usage.attr,
	&attr_memory_pressure.attr,
	NULL,
};

static const struct attribute_group vpu_attr_group = {
	.attrs = vpu_attrs,
};

/* ---- init / cleanup ---------------------------------------------------- */

int vpu_sysfs_init(struct vpu_device *vdev)
{
	int ret;

	sysfs_vdev = vdev;

	vdev->sysfs_kobj = kobject_create_and_add("straylight-vpu",
						  kernel_kobj);
	if (!vdev->sysfs_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(vdev->sysfs_kobj, &vpu_attr_group);
	if (ret) {
		kobject_put(vdev->sysfs_kobj);
		vdev->sysfs_kobj = NULL;
		return ret;
	}

	return 0;
}

void vpu_sysfs_cleanup(struct vpu_device *vdev)
{
	if (!vdev->sysfs_kobj)
		return;

	sysfs_remove_group(vdev->sysfs_kobj, &vpu_attr_group);
	kobject_put(vdev->sysfs_kobj);
	vdev->sysfs_kobj = NULL;
	sysfs_vdev = NULL;
}
