// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Hypervisor Main Module
 * Copyright (C) 2026 StrayLight Systems
 *
 * Module init/exit, VT-x capability check, misc-device registration.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>

#include "hv.h"

#define HV_DEV_NAME     "straylight-hv"

static struct hv_device *g_hdev;

/* ---- CPU feature checks ------------------------------------------------ */

#define MSR_IA32_FEATURE_CONTROL        0x0000003A
#define FEATURE_CONTROL_LOCKED          (1ULL << 0)
#define FEATURE_CONTROL_VMXON_OUTSIDE   (1ULL << 2)

#define MSR_IA32_VMX_BASIC              0x00000480

static bool hv_check_vmx_support(void)
{
	u32 ecx;
	u64 feat_ctl;

	/* CPUID.1:ECX.VMX[bit 5] */
	ecx = cpuid_ecx(1);
	if (!(ecx & (1U << 5))) {
		pr_info("straylight-hv: CPU does not support VMX\n");
		return false;
	}

	/* Check IA32_FEATURE_CONTROL MSR */
	rdmsrl_safe(MSR_IA32_FEATURE_CONTROL, &feat_ctl);

	if (feat_ctl & FEATURE_CONTROL_LOCKED) {
		if (!(feat_ctl & FEATURE_CONTROL_VMXON_OUTSIDE)) {
			pr_info("straylight-hv: VMX locked out by BIOS\n");
			return false;
		}
	}

	pr_info("straylight-hv: VMX support detected\n");
	return true;
}

/* ---- VM management ----------------------------------------------------- */

static struct hv_vm_context *hv_find_vm(struct hv_device *hdev, u32 vm_id)
{
	struct hv_vm_context *vm;

	list_for_each_entry(vm, &hdev->vm_list, list) {
		if (vm->vm_id == vm_id)
			return vm;
	}
	return NULL;
}

static long hv_ioctl_create_vm(struct hv_device *hdev, unsigned long arg)
{
	struct hv_create_vm_req req;
	struct hv_vm_context *vm;
	unsigned int i;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.nr_vcpus == 0 || req.nr_vcpus > HV_MAX_VCPUS)
		return -EINVAL;

	if (req.mem_size == 0 || req.mem_size > (64ULL << 30))
		return -EINVAL;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	mutex_init(&vm->lock);
	vm->nr_vcpus = req.nr_vcpus;
	vm->mem_size = req.mem_size;

	/* Allocate guest physical memory backing */
	vm->guest_mem = vzalloc(req.mem_size);
	if (!vm->guest_mem) {
		ret = -ENOMEM;
		goto err_vm;
	}

	/* Initialise EPT */
	ret = hv_ept_init(&vm->ept);
	if (ret)
		goto err_mem;

	/* Map guest memory into EPT */
	{
		u64 gpa;
		unsigned long hva;

		for (gpa = 0; gpa < req.mem_size; gpa += PAGE_SIZE) {
			struct page *page;

			hva = (unsigned long)vm->guest_mem + gpa;
			page = vmalloc_to_page((void *)hva);
			if (!page) {
				ret = -EFAULT;
				goto err_ept;
			}

			ret = hv_ept_map_page(&vm->ept, gpa,
					      page_to_phys(page),
					      EPT_PTE_READ | EPT_PTE_WRITE |
					      EPT_PTE_EXEC);
			if (ret)
				goto err_ept;
		}
	}

	/* Initialise VCPUs */
	for (i = 0; i < vm->nr_vcpus; i++) {
		struct hv_vcpu *vcpu = &vm->vcpus[i];

		vcpu->id = i;
		vcpu->launched = false;

		ret = hv_vmcs_alloc(vcpu);
		if (ret)
			goto err_vcpus;

		ret = hv_vmcs_setup(vcpu, vm);
		if (ret) {
			hv_vmcs_free(vcpu);
			goto err_vcpus;
		}
	}

	/* Initialise profiling counters */
	hv_profile_init(&vm->profile);

	mutex_lock(&hdev->vm_lock);
	vm->vm_id = hdev->next_vm_id++;
	list_add_tail(&vm->list, &hdev->vm_list);
	mutex_unlock(&hdev->vm_lock);

	req.vm_id = vm->vm_id;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	pr_info("straylight-hv: VM %u created (%u VCPUs, %llu MiB)\n",
		vm->vm_id, vm->nr_vcpus, vm->mem_size >> 20);
	return 0;

err_vcpus:
	while (i-- > 0)
		hv_vmcs_free(&vm->vcpus[i]);
err_ept:
	hv_ept_destroy(&vm->ept);
err_mem:
	vfree(vm->guest_mem);
err_vm:
	kfree(vm);
	return ret;
}

static long hv_ioctl_destroy_vm(struct hv_device *hdev, unsigned long arg)
{
	struct hv_destroy_vm_req req;
	struct hv_vm_context *vm;
	unsigned int i;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	if (!vm) {
		mutex_unlock(&hdev->vm_lock);
		return -ENOENT;
	}
	list_del(&vm->list);
	mutex_unlock(&hdev->vm_lock);

	for (i = 0; i < vm->nr_vcpus; i++)
		hv_vmcs_free(&vm->vcpus[i]);

	hv_ept_destroy(&vm->ept);
	vfree(vm->guest_mem);
	kfree(vm);

	pr_info("straylight-hv: VM %u destroyed\n", req.vm_id);
	return 0;
}

static long hv_ioctl_run_vm(struct hv_device *hdev, unsigned long arg)
{
	struct hv_run_vm_req req;
	struct hv_vm_context *vm;
	struct hv_vcpu *vcpu;
	u64 start_cycles, end_cycles, delta;
	u32 exit_reason;
	u64 exit_qual;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	mutex_unlock(&hdev->vm_lock);

	if (!vm)
		return -ENOENT;

	/* Use VCPU 0 for simplicity in this single-step ioctl */
	vcpu = &vm->vcpus[0];

	/*
	 * In a real implementation this would execute VMLAUNCH/VMRESUME.
	 * We simulate the VM-exit cycle: read exit reason, handle it,
	 * record profiling data.
	 */
	start_cycles = rdtsc();

	/*
	 * Simulated exit — in production this is replaced by actual
	 * VMX non-root execution.  We read back the fields that would
	 * be populated by the hardware after VM-exit.
	 */
	exit_reason = EXIT_REASON_CPUID; /* simulated */
	exit_qual   = 0;

	ret = hv_handle_exit(vm, vcpu, exit_reason, exit_qual);

	end_cycles = rdtsc();
	delta = end_cycles - start_cycles;

	hv_profile_record_exit(&vm->profile, exit_reason, delta);

	req.exit_reason       = exit_reason;
	req.exit_qualification = exit_qual;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return ret;
}

static long hv_ioctl_set_regs(struct hv_device *hdev, unsigned long arg)
{
	struct hv_regs_req req;
	struct hv_vm_context *vm;
	struct hv_vcpu *vcpu;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	mutex_unlock(&hdev->vm_lock);

	if (!vm)
		return -ENOENT;

	if (req.vcpu_id >= vm->nr_vcpus)
		return -EINVAL;

	vcpu = &vm->vcpus[req.vcpu_id];

	mutex_lock(&vm->lock);
	memcpy(&vcpu->regs, &req, sizeof(req));
	mutex_unlock(&vm->lock);

	return 0;
}

static long hv_ioctl_get_regs(struct hv_device *hdev, unsigned long arg)
{
	struct hv_regs_req req;
	struct hv_vm_context *vm;
	struct hv_vcpu *vcpu;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	mutex_unlock(&hdev->vm_lock);

	if (!vm)
		return -ENOENT;

	if (req.vcpu_id >= vm->nr_vcpus)
		return -EINVAL;

	vcpu = &vm->vcpus[req.vcpu_id];

	mutex_lock(&vm->lock);
	memcpy(&req, &vcpu->regs, sizeof(req));
	mutex_unlock(&vm->lock);

	req.vm_id   = vm->vm_id;
	req.vcpu_id = vcpu->id;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long hv_ioctl_map_memory(struct hv_device *hdev, unsigned long arg)
{
	struct hv_map_mem_req req;
	struct hv_vm_context *vm;
	u64 gpa, hpa;
	unsigned long hva;
	struct page *page;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	mutex_unlock(&hdev->vm_lock);

	if (!vm)
		return -ENOENT;

	/* Map each page of the range */
	for (gpa = req.gpa; gpa < req.gpa + req.size; gpa += PAGE_SIZE) {
		hva = req.hva + (gpa - req.gpa);

		ret = get_user_pages_fast(hva, 1, FOLL_WRITE, &page);
		if (ret != 1)
			return -EFAULT;

		hpa = page_to_phys(page);

		ret = hv_ept_map_page(&vm->ept, gpa, hpa,
				      EPT_PTE_READ | EPT_PTE_WRITE |
				      EPT_PTE_EXEC);
		put_page(page);
		if (ret)
			return ret;
	}

	return 0;
}

static long hv_ioctl_get_stats(struct hv_device *hdev, unsigned long arg)
{
	struct hv_stats_req req;
	struct hv_vm_context *vm;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&hdev->vm_lock);
	vm = hv_find_vm(hdev, req.vm_id);
	mutex_unlock(&hdev->vm_lock);

	if (!vm)
		return -ENOENT;

	req.cpuid_exits   = atomic64_read(&vm->profile.cpuid_exits);
	req.msr_exits     = atomic64_read(&vm->profile.msr_exits);
	req.io_exits      = atomic64_read(&vm->profile.io_exits);
	req.ept_violations = atomic64_read(&vm->profile.ept_violations);
	req.total_exits   = atomic64_read(&vm->profile.total_exits);
	req.total_run_ns  = atomic64_read(&vm->profile.total_run_cycles);

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* ---- file operations --------------------------------------------------- */

static int hv_open(struct inode *inode, struct file *filp)
{
	filp->private_data = g_hdev;
	return 0;
}

static int hv_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long hv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct hv_device *hdev = filp->private_data;

	switch (cmd) {
	case HV_IOC_CREATE_VM:
		return hv_ioctl_create_vm(hdev, arg);
	case HV_IOC_DESTROY_VM:
		return hv_ioctl_destroy_vm(hdev, arg);
	case HV_IOC_RUN_VM:
		return hv_ioctl_run_vm(hdev, arg);
	case HV_IOC_SET_REGS:
		return hv_ioctl_set_regs(hdev, arg);
	case HV_IOC_GET_REGS:
		return hv_ioctl_get_regs(hdev, arg);
	case HV_IOC_MAP_MEMORY:
		return hv_ioctl_map_memory(hdev, arg);
	case HV_IOC_GET_STATS:
		return hv_ioctl_get_stats(hdev, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations hv_fops = {
	.owner          = THIS_MODULE,
	.open           = hv_open,
	.release        = hv_release,
	.unlocked_ioctl = hv_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ---- module init / exit ------------------------------------------------ */

static int __init hv_module_init(void)
{
	int ret;

	g_hdev = kzalloc(sizeof(*g_hdev), GFP_KERNEL);
	if (!g_hdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&g_hdev->vm_list);
	mutex_init(&g_hdev->vm_lock);
	g_hdev->next_vm_id = 1;

	/* Check VT-x — informational, we still load even without it
	 * so the userspace tooling can query capabilities. */
	g_hdev->vmx_enabled = hv_check_vmx_support();

	g_hdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	g_hdev->miscdev.name  = HV_DEV_NAME;
	g_hdev->miscdev.fops  = &hv_fops;

	ret = misc_register(&g_hdev->miscdev);
	if (ret) {
		pr_err("straylight-hv: misc_register failed (%d)\n", ret);
		kfree(g_hdev);
		g_hdev = NULL;
		return ret;
	}

	ret = hv_profiler_proc_init(g_hdev);
	if (ret)
		pr_warn("straylight-hv: proc init failed (%d)\n", ret);

	pr_info("straylight-hv: module loaded (VMX %s)\n",
		g_hdev->vmx_enabled ? "enabled" : "not available");
	return 0;
}

static void __exit hv_module_exit(void)
{
	struct hv_vm_context *vm, *tmp;

	if (!g_hdev)
		return;

	hv_profiler_proc_cleanup(g_hdev);

	/* Tear down any remaining VMs */
	mutex_lock(&g_hdev->vm_lock);
	list_for_each_entry_safe(vm, tmp, &g_hdev->vm_list, list) {
		unsigned int i;

		pr_warn("straylight-hv: cleaning up leaked VM %u\n", vm->vm_id);
		for (i = 0; i < vm->nr_vcpus; i++)
			hv_vmcs_free(&vm->vcpus[i]);
		hv_ept_destroy(&vm->ept);
		vfree(vm->guest_mem);
		list_del(&vm->list);
		kfree(vm);
	}
	mutex_unlock(&g_hdev->vm_lock);

	misc_deregister(&g_hdev->miscdev);
	kfree(g_hdev);
	g_hdev = NULL;

	pr_info("straylight-hv: module unloaded\n");
}

module_init(hv_module_init);
module_exit(hv_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StrayLight Systems");
MODULE_DESCRIPTION("StrayLight Hypervisor KVM Extensions");
MODULE_VERSION("1.0.0");
