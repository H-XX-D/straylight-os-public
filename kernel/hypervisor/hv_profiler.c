// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Hypervisor Profiler
 * Copyright (C) 2026 StrayLight Systems
 *
 * Per-VM exit counters, rdtsc-based cycle profiling, /proc interface.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "hv.h"

/* ---- Profile counter operations ---------------------------------------- */

void hv_profile_init(struct hv_profile *prof)
{
	atomic64_set(&prof->cpuid_exits, 0);
	atomic64_set(&prof->msr_exits, 0);
	atomic64_set(&prof->io_exits, 0);
	atomic64_set(&prof->ept_violations, 0);
	atomic64_set(&prof->other_exits, 0);
	atomic64_set(&prof->total_exits, 0);
	atomic64_set(&prof->total_run_cycles, 0);
}

void hv_profile_record_exit(struct hv_profile *prof, u32 reason, u64 cycles)
{
	atomic64_inc(&prof->total_exits);
	atomic64_add(cycles, &prof->total_run_cycles);

	switch (reason) {
	case EXIT_REASON_CPUID:
		atomic64_inc(&prof->cpuid_exits);
		break;
	case EXIT_REASON_MSR_READ:
	case EXIT_REASON_MSR_WRITE:
		atomic64_inc(&prof->msr_exits);
		break;
	case EXIT_REASON_IO_INSTRUCTION:
		atomic64_inc(&prof->io_exits);
		break;
	case EXIT_REASON_EPT_VIOLATION:
	case EXIT_REASON_EPT_MISCONFIG:
		atomic64_inc(&prof->ept_violations);
		break;
	default:
		atomic64_inc(&prof->other_exits);
		break;
	}
}

/* ---- /proc/straylight/hv interface ------------------------------------- */

static struct hv_device *proc_hdev;

static int hv_proc_show(struct seq_file *m, void *v)
{
	struct hv_vm_context *vm;
	u64 total_cycles, total_exits, avg_cycles;

	if (!proc_hdev)
		return -ENODEV;

	seq_puts(m, "StrayLight Hypervisor Profiling Data\n");
	seq_puts(m, "====================================\n\n");
	seq_printf(m, "VMX support: %s\n\n",
		   proc_hdev->vmx_enabled ? "yes" : "no");

	mutex_lock(&proc_hdev->vm_lock);

	if (list_empty(&proc_hdev->vm_list)) {
		seq_puts(m, "No active VMs\n");
		mutex_unlock(&proc_hdev->vm_lock);
		return 0;
	}

	list_for_each_entry(vm, &proc_hdev->vm_list, list) {
		struct hv_profile *p = &vm->profile;

		total_exits  = atomic64_read(&p->total_exits);
		total_cycles = atomic64_read(&p->total_run_cycles);

		if (total_exits > 0)
			avg_cycles = total_cycles / total_exits;
		else
			avg_cycles = 0;

		seq_printf(m, "VM %u (%u VCPUs, %llu MiB)\n",
			   vm->vm_id, vm->nr_vcpus, vm->mem_size >> 20);
		seq_puts(m, "  Exit Type          Count\n");
		seq_puts(m, "  ---------          -----\n");
		seq_printf(m, "  CPUID              %lld\n",
			   atomic64_read(&p->cpuid_exits));
		seq_printf(m, "  MSR                %lld\n",
			   atomic64_read(&p->msr_exits));
		seq_printf(m, "  I/O                %lld\n",
			   atomic64_read(&p->io_exits));
		seq_printf(m, "  EPT violations     %lld\n",
			   atomic64_read(&p->ept_violations));
		seq_printf(m, "  Other              %lld\n",
			   atomic64_read(&p->other_exits));
		seq_printf(m, "  Total              %lld\n",
			   total_exits);
		seq_printf(m, "  Total cycles       %llu\n",
			   total_cycles);
		seq_printf(m, "  Avg cycles/exit    %llu\n\n",
			   avg_cycles);
	}

	mutex_unlock(&proc_hdev->vm_lock);
	return 0;
}

static int hv_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hv_proc_show, NULL);
}

static const struct proc_ops hv_proc_ops = {
	.proc_open    = hv_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- Per-VM detailed stats --------------------------------------------- */

static int hv_vm_proc_show(struct seq_file *m, void *v)
{
	struct hv_vm_context *vm;
	u32 target_vm_id;

	if (!proc_hdev)
		return -ENODEV;

	target_vm_id = (u32)(unsigned long)m->private;

	mutex_lock(&proc_hdev->vm_lock);

	list_for_each_entry(vm, &proc_hdev->vm_list, list) {
		if (vm->vm_id == target_vm_id) {
			struct hv_profile *p = &vm->profile;
			u64 total = atomic64_read(&p->total_exits);
			u64 cycles = atomic64_read(&p->total_run_cycles);

			seq_printf(m, "vm_id=%u\n", vm->vm_id);
			seq_printf(m, "nr_vcpus=%u\n", vm->nr_vcpus);
			seq_printf(m, "mem_mib=%llu\n", vm->mem_size >> 20);
			seq_printf(m, "cpuid_exits=%lld\n",
				   atomic64_read(&p->cpuid_exits));
			seq_printf(m, "msr_exits=%lld\n",
				   atomic64_read(&p->msr_exits));
			seq_printf(m, "io_exits=%lld\n",
				   atomic64_read(&p->io_exits));
			seq_printf(m, "ept_violations=%lld\n",
				   atomic64_read(&p->ept_violations));
			seq_printf(m, "other_exits=%lld\n",
				   atomic64_read(&p->other_exits));
			seq_printf(m, "total_exits=%llu\n", total);
			seq_printf(m, "total_cycles=%llu\n", cycles);

			if (total > 0)
				seq_printf(m, "avg_cycles_per_exit=%llu\n",
					   cycles / total);
			else
				seq_puts(m, "avg_cycles_per_exit=0\n");

			/* Exit frequency breakdown as percentages */
			if (total > 0) {
				seq_printf(m, "cpuid_pct=%llu\n",
					   atomic64_read(&p->cpuid_exits) * 100 / total);
				seq_printf(m, "msr_pct=%llu\n",
					   atomic64_read(&p->msr_exits) * 100 / total);
				seq_printf(m, "io_pct=%llu\n",
					   atomic64_read(&p->io_exits) * 100 / total);
				seq_printf(m, "ept_pct=%llu\n",
					   atomic64_read(&p->ept_violations) * 100 / total);
			}

			mutex_unlock(&proc_hdev->vm_lock);
			return 0;
		}
	}

	mutex_unlock(&proc_hdev->vm_lock);
	return -ENOENT;
}

static int hv_vm_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hv_vm_proc_show, pde_data(inode));
}

static const struct proc_ops hv_vm_proc_ops = {
	.proc_open    = hv_vm_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- Reset stats via /proc write --------------------------------------- */

static ssize_t hv_proc_reset_write(struct file *file,
				   const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct hv_vm_context *vm;

	if (!proc_hdev)
		return -ENODEV;

	mutex_lock(&proc_hdev->vm_lock);
	list_for_each_entry(vm, &proc_hdev->vm_list, list)
		hv_profile_init(&vm->profile);
	mutex_unlock(&proc_hdev->vm_lock);

	pr_info("straylight-hv: profiling counters reset\n");
	return count;
}

static int hv_proc_reset_show(struct seq_file *m, void *v)
{
	seq_puts(m, "Write anything to reset all VM profiling counters\n");
	return 0;
}

static int hv_proc_reset_open(struct inode *inode, struct file *file)
{
	return single_open(file, hv_proc_reset_show, NULL);
}

static const struct proc_ops hv_reset_proc_ops = {
	.proc_open    = hv_proc_reset_open,
	.proc_read    = seq_read,
	.proc_write   = hv_proc_reset_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- Init / cleanup ---------------------------------------------------- */

int hv_profiler_proc_init(struct hv_device *hdev)
{
	proc_hdev = hdev;

	hdev->proc_dir = proc_mkdir("straylight/hv", NULL);
	if (!hdev->proc_dir) {
		/* Try creating parent first */
		proc_mkdir("straylight", NULL);
		hdev->proc_dir = proc_mkdir("straylight/hv", NULL);
		if (!hdev->proc_dir)
			return -ENOMEM;
	}

	if (!proc_create("stats", 0444, hdev->proc_dir, &hv_proc_ops))
		goto err;

	if (!proc_create("reset", 0200, hdev->proc_dir, &hv_reset_proc_ops))
		goto err;

	pr_info("straylight-hv: /proc/straylight/hv/ created\n");
	return 0;

err:
	remove_proc_subtree("straylight/hv", NULL);
	hdev->proc_dir = NULL;
	return -ENOMEM;
}

void hv_profiler_proc_cleanup(struct hv_device *hdev)
{
	if (hdev->proc_dir) {
		remove_proc_subtree("straylight/hv", NULL);
		hdev->proc_dir = NULL;
	}
	proc_hdev = NULL;
}
