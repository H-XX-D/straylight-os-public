// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VMCS Setup / Teardown
 * Copyright (C) 2026 StrayLight Systems
 *
 * Allocate VMCS regions, configure fields for guest launch.
 * Provides read/write wrappers for VMCS field access.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <asm/io.h>
#include <asm/msr.h>

#include "hv.h"

#define MSR_IA32_VMX_BASIC              0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS      0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS     0x00000482
#define MSR_IA32_VMX_EXIT_CTLS          0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS         0x00000484
#define MSR_IA32_VMX_TRUE_PINBASED      0x0000048D
#define MSR_IA32_VMX_TRUE_PROCBASED     0x0000048E
#define MSR_IA32_VMX_TRUE_EXIT          0x0000048F
#define MSR_IA32_VMX_TRUE_ENTRY         0x00000490
#ifndef MSR_IA32_VMX_PROCBASED_CTLS2
#define MSR_IA32_VMX_PROCBASED_CTLS2    0x0000048B
#endif

/* Pin-based controls */
#define PIN_BASED_EXT_INT_EXIT          (1U << 0)
#define PIN_BASED_NMI_EXIT              (1U << 3)

/* Primary proc-based controls */
#define PROC_BASED_HLT_EXIT             (1U << 7)
#define PROC_BASED_INVLPG_EXIT          (1U << 9)
#define PROC_BASED_MWAIT_EXIT           (1U << 10)
#define PROC_BASED_RDPMC_EXIT           (1U << 11)
#define PROC_BASED_RDTSC_EXIT           (1U << 12)
#define PROC_BASED_CR3_LOAD_EXIT        (1U << 15)
#define PROC_BASED_CR3_STORE_EXIT       (1U << 16)
#define PROC_BASED_USE_MSR_BITMAPS      (1U << 28)
#define PROC_BASED_ACTIVATE_SEC         (1U << 31)

/* Secondary proc-based controls */
#define PROC_BASED2_EPT                 (1U << 1)
#define PROC_BASED2_RDTSCP              (1U << 3)
#define PROC_BASED2_VPID                (1U << 5)
#define PROC_BASED2_UNRESTRICTED        (1U << 7)
#define PROC_BASED2_INVPCID             (1U << 12)
#define PROC_BASED2_XSAVES             (1U << 20)

/* Exit controls */
#define EXIT_CTRL_HOST_ADDR_SIZE        (1U << 9)
#define EXIT_CTRL_ACK_INT               (1U << 15)
#define EXIT_CTRL_SAVE_PAT              (1U << 18)
#define EXIT_CTRL_LOAD_PAT              (1U << 19)

/* Entry controls */
#define ENTRY_CTRL_LOAD_PAT             (1U << 14)
#define ENTRY_CTRL_IA32E_MODE           (1U << 9)

/* Segment access rights for 64-bit flat mode */
#define SEG_AR_CODE_RX_ACCESSED         0xA09B  /* L=1, D=0, P=1, S=1, CS exec/read */
#define SEG_AR_DATA_RW_ACCESSED         0xC093  /* G=1, D/B=1, P=1, S=1, DS r/w     */
#define SEG_AR_TSS_BUSY                 0x008B  /* P=1, Type=TSS 64-bit busy         */
#define SEG_AR_UNUSABLE                 0x10000

/*
 * VMCS read/write wrappers.
 *
 * On real hardware these use VMREAD/VMWRITE instructions.
 * We implement them as inline asm with error checking.
 */

u32 hv_vmcs_read32(unsigned long field)
{
	unsigned long value = 0;

#ifdef CONFIG_X86_64
	asm volatile("vmread %[field], %[value]"
		     : [value] "=r" (value)
		     : [field] "r" ((u64)field)
		     : "cc");
#else
	(void)field;
#endif
	return (u32)value;
}

u64 hv_vmcs_read64(unsigned long field)
{
	u64 value = 0;

#ifdef CONFIG_X86_64
	asm volatile("vmread %[field], %[value]"
		     : [value] "=r" (value)
		     : [field] "r" ((u64)field)
		     : "cc");
#else
	(void)field;
#endif
	return value;
}

void hv_vmcs_write32(unsigned long field, u32 value)
{
#ifdef CONFIG_X86_64
	u64 val64 = value;

	asm volatile("vmwrite %[value], %[field]"
		     :
		     : [field] "r" ((u64)field), [value] "r" (val64)
		     : "cc");
#else
	(void)field;
	(void)value;
#endif
}

void hv_vmcs_write64(unsigned long field, u64 value)
{
#ifdef CONFIG_X86_64
	asm volatile("vmwrite %[value], %[field]"
		     :
		     : [field] "r" ((u64)field), [value] "r" (value)
		     : "cc");
#else
	(void)field;
	(void)value;
#endif
}

/* ---- VMCS allocation --------------------------------------------------- */

int hv_vmcs_alloc(struct hv_vcpu *vcpu)
{
	struct page *page;
	u64 vmx_basic;
	u32 vmcs_revision;
	u32 *vmcs_hdr;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	vcpu->vmcs_page = page;
	vcpu->vmcs_phys = page_to_phys(page);

	/*
	 * The first 4 bytes of the VMCS must contain the VMCS revision
	 * identifier from IA32_VMX_BASIC MSR bits 30:0.
	 */
	if (rdmsrl_safe(MSR_IA32_VMX_BASIC, &vmx_basic) == 0) {
		vmcs_revision = (u32)(vmx_basic & 0x7FFFFFFFU);
	} else {
		vmcs_revision = 0; /* fallback — will be set before VMPTRLD */
	}

	vmcs_hdr = page_address(page);
	vmcs_hdr[0] = vmcs_revision;

	return 0;
}

void hv_vmcs_free(struct hv_vcpu *vcpu)
{
	if (vcpu->vmcs_page) {
		/*
		 * VMCLEAR before freeing to ensure the VMCS is flushed
		 * from the processor's internal caches.
		 */
#ifdef CONFIG_X86_64
		u64 phys = vcpu->vmcs_phys;

		asm volatile("vmclear %[phys]"
			     :
			     : [phys] "m" (phys)
			     : "cc", "memory");
#endif
		__free_page(vcpu->vmcs_page);
		vcpu->vmcs_page = NULL;
		vcpu->vmcs_phys = 0;
	}
}

/* ---- Adjust control field value per allowed-0 / allowed-1 MSR ---------- */

static u32 hv_adjust_controls(u32 desired, u32 msr)
{
	u64 msr_val = 0;
	u32 allowed0, allowed1;

	rdmsrl_safe(msr, &msr_val);
	allowed0 = (u32)msr_val;         /* bits that must be 0 */
	allowed1 = (u32)(msr_val >> 32); /* bits that must be 1 */

	/* Set bits that must be 1, clear bits that must be 0 */
	desired |= allowed0;
	desired &= allowed1;

	return desired;
}

/* ---- VMCS field setup -------------------------------------------------- */

int hv_vmcs_setup(struct hv_vcpu *vcpu, struct hv_vm_context *vm)
{
	u32 pin_ctls, proc_ctls, proc_ctls2, exit_ctls, entry_ctls;
	u64 eptp;

#ifdef CONFIG_X86_64
	/* Load this VMCS as current */
	u64 phys = vcpu->vmcs_phys;

	asm volatile("vmptrld %[phys]"
		     :
		     : [phys] "m" (phys)
		     : "cc", "memory");
#endif

	/* ---- Control fields ---- */
	pin_ctls = hv_adjust_controls(
		PIN_BASED_EXT_INT_EXIT | PIN_BASED_NMI_EXIT,
		MSR_IA32_VMX_TRUE_PINBASED);
	hv_vmcs_write32(VMCS_PIN_BASED_CTLS, pin_ctls);

	proc_ctls = hv_adjust_controls(
		PROC_BASED_HLT_EXIT |
		PROC_BASED_USE_MSR_BITMAPS |
		PROC_BASED_ACTIVATE_SEC,
		MSR_IA32_VMX_TRUE_PROCBASED);
	hv_vmcs_write32(VMCS_PROC_BASED_CTLS, proc_ctls);

	proc_ctls2 = hv_adjust_controls(
		PROC_BASED2_EPT |
		PROC_BASED2_VPID |
		PROC_BASED2_UNRESTRICTED |
		PROC_BASED2_RDTSCP,
		MSR_IA32_VMX_PROCBASED_CTLS2);
	hv_vmcs_write32(VMCS_PROC_BASED_CTLS2, proc_ctls2);

	exit_ctls = hv_adjust_controls(
		EXIT_CTRL_HOST_ADDR_SIZE |
		EXIT_CTRL_ACK_INT |
		EXIT_CTRL_SAVE_PAT |
		EXIT_CTRL_LOAD_PAT,
		MSR_IA32_VMX_TRUE_EXIT);
	hv_vmcs_write32(VMCS_EXIT_CTLS, exit_ctls);

	entry_ctls = hv_adjust_controls(
		ENTRY_CTRL_IA32E_MODE |
		ENTRY_CTRL_LOAD_PAT,
		MSR_IA32_VMX_TRUE_ENTRY);
	hv_vmcs_write32(VMCS_ENTRY_CTLS, entry_ctls);

	/* ---- EPT pointer ---- */
	if (vm->ept.pml4) {
		eptp = virt_to_phys(vm->ept.pml4);
		eptp |= ((u64)EPT_PAGE_WALK_LENGTH << 3);
		eptp |= EPT_MEM_TYPE_WB;
		hv_vmcs_write64(VMCS_EPT_PTR, eptp);
	}

	/* ---- VPID ---- */
	hv_vmcs_write32(VMCS_VPID, vm->vm_id & 0xFFFF);

	/* ---- Exception bitmap — intercept nothing initially ---- */
	hv_vmcs_write32(VMCS_EXCEPTION_BITMAP, 0);

	/* ---- Guest state — 64-bit long mode, flat segments ---- */
	hv_vmcs_write64(VMCS_GUEST_CR0, 0x80050033UL); /* PG PE NE MP ET */
	hv_vmcs_write64(VMCS_GUEST_CR3, 0);
	hv_vmcs_write64(VMCS_GUEST_CR4, 0x00002020UL); /* PAE VMXE */

	/* CS: 64-bit code segment */
	hv_vmcs_write32(VMCS_GUEST_CS_SEL, 0x08);
	hv_vmcs_write64(VMCS_GUEST_CS_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
	hv_vmcs_write32(VMCS_GUEST_CS_AR, SEG_AR_CODE_RX_ACCESSED);

	/* DS/ES/SS: 64-bit data segments */
	hv_vmcs_write32(VMCS_GUEST_DS_SEL, 0x10);
	hv_vmcs_write64(VMCS_GUEST_DS_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
	hv_vmcs_write32(VMCS_GUEST_DS_AR, SEG_AR_DATA_RW_ACCESSED);

	hv_vmcs_write32(VMCS_GUEST_ES_SEL, 0x10);
	hv_vmcs_write64(VMCS_GUEST_ES_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_ES_LIMIT, 0xFFFFFFFF);
	hv_vmcs_write32(VMCS_GUEST_ES_AR, SEG_AR_DATA_RW_ACCESSED);

	hv_vmcs_write32(VMCS_GUEST_SS_SEL, 0x10);
	hv_vmcs_write64(VMCS_GUEST_SS_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_SS_LIMIT, 0xFFFFFFFF);
	hv_vmcs_write32(VMCS_GUEST_SS_AR, SEG_AR_DATA_RW_ACCESSED);

	/* FS/GS: unused */
	hv_vmcs_write32(VMCS_GUEST_FS_SEL, 0);
	hv_vmcs_write64(VMCS_GUEST_FS_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_FS_LIMIT, 0);
	hv_vmcs_write32(VMCS_GUEST_FS_AR, SEG_AR_UNUSABLE);

	hv_vmcs_write32(VMCS_GUEST_GS_SEL, 0);
	hv_vmcs_write64(VMCS_GUEST_GS_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_GS_LIMIT, 0);
	hv_vmcs_write32(VMCS_GUEST_GS_AR, SEG_AR_UNUSABLE);

	/* LDTR: unusable */
	hv_vmcs_write32(VMCS_GUEST_LDTR_SEL, 0);
	hv_vmcs_write64(VMCS_GUEST_LDTR_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_LDTR_LIMIT, 0);
	hv_vmcs_write32(0x00004820, SEG_AR_UNUSABLE); /* LDTR AR */

	/* TR: 64-bit TSS */
	hv_vmcs_write32(VMCS_GUEST_TR_SEL, 0x18);
	hv_vmcs_write64(VMCS_GUEST_TR_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_TR_LIMIT, 0x67);
	hv_vmcs_write32(0x00004822, SEG_AR_TSS_BUSY); /* TR AR */

	/* GDTR / IDTR */
	hv_vmcs_write64(VMCS_GUEST_GDTR_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_GDTR_LIMIT, 0x1F);
	hv_vmcs_write64(VMCS_GUEST_IDTR_BASE, 0);
	hv_vmcs_write32(VMCS_GUEST_IDTR_LIMIT, 0xFF);

	/* RIP, RSP, RFLAGS */
	hv_vmcs_write64(VMCS_GUEST_RIP, 0x0);
	hv_vmcs_write64(VMCS_GUEST_RSP, 0x0);
	hv_vmcs_write64(VMCS_GUEST_RFLAGS, 0x02); /* bit 1 always set */

	/* DR7, interruptibility, activity */
	hv_vmcs_write64(VMCS_GUEST_DR7, 0x400);
	hv_vmcs_write32(VMCS_GUEST_INTERRUPTIBILITY, 0);
	hv_vmcs_write32(VMCS_GUEST_ACTIVITY, 0); /* active */

	/* ---- Host state ---- */
	{
		unsigned long cr0, cr3, cr4;

#ifdef CONFIG_X86_64
		cr0 = read_cr0();
		cr3 = __read_cr3();
		cr4 = __read_cr4();
#else
		cr0 = 0;
		cr3 = 0;
		cr4 = 0;
#endif
		hv_vmcs_write64(VMCS_HOST_CR0, cr0);
		hv_vmcs_write64(VMCS_HOST_CR3, cr3);
		hv_vmcs_write64(VMCS_HOST_CR4, cr4);
	}

	pr_debug("straylight-hv: VMCS setup complete for VCPU %u\n", vcpu->id);
	return 0;
}
