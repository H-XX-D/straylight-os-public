// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — VM-Exit Handler Dispatch
 * Copyright (C) 2026 StrayLight Systems
 *
 * Handles CPUID, MSR read/write, IO, HLT, EPT violations, and other exits.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/io.h>

#include "hv.h"

/* ---- CPUID emulation --------------------------------------------------- */

/*
 * StrayLight-branded CPUID responses.
 * Leaf 0x40000000 returns the hypervisor vendor string "StraylightHV".
 */

struct cpuid_result {
	u32 eax, ebx, ecx, edx;
};

static void hv_emulate_cpuid(struct hv_vcpu *vcpu)
{
	u32 leaf = (u32)vcpu->regs.rax;
	u32 subleaf = (u32)vcpu->regs.rcx;
	struct cpuid_result res = {0, 0, 0, 0};

	switch (leaf) {
	case 0x00000000:
		/* Max standard leaf + vendor string */
		res.eax = 0x00000016; /* up to leaf 0x16 */
		res.ebx = 0x756E6547; /* "Genu" */
		res.edx = 0x49656E69; /* "ineI" */
		res.ecx = 0x6C65746E; /* "ntel" */
		break;

	case 0x00000001:
		/*
		 * Feature flags — report a virtualised Skylake-like CPU.
		 * Mask out VMX (bit 5 of ECX) since we're inside a VM.
		 */
		res.eax = 0x000506E3; /* Family 6, Model 5E, Stepping 3 */
		res.ebx = (vcpu->id << 24) | (1 << 16) | (0x40 << 8);
		res.ecx = 0x7FFAFBBF & ~(1U << 5); /* clear VMX */
		res.edx = 0xBFEBFBFF;
		break;

	case 0x40000000:
		/* Hypervisor CPUID interface — vendor "StraylightHV" */
		res.eax = 0x40000001; /* max hypervisor leaf */
		res.ebx = 0x61727453; /* "Stra" */
		res.ecx = 0x67696C79; /* "ylig" */
		res.edx = 0x56487468; /* "htHV" */
		break;

	case 0x40000001:
		/* Hypervisor features */
		res.eax = 0x00000001; /* version 1 */
		res.ebx = 0;
		res.ecx = 0;
		res.edx = 0;
		break;

	case 0x80000000:
		/* Max extended leaf */
		res.eax = 0x80000008;
		break;

	case 0x80000001:
		/* Extended features */
		res.ecx = (1U << 0) | /* LAHF/SAHF */
			  (1U << 5);  /* LZCNT */
		res.edx = (1U << 11) | /* SYSCALL */
			  (1U << 20) | /* NX */
			  (1U << 26) | /* 1G pages */
			  (1U << 27) | /* RDTSCP */
			  (1U << 29);  /* Long mode */
		break;

	case 0x80000002:
		/* Processor brand string part 1: "Stra" "yLig" "ht V" "PU " */
		res.eax = 0x61727453;
		res.ebx = 0x67694C79;
		res.ecx = 0x56207468;
		res.edx = 0x00205550;
		break;

	case 0x80000003:
		/* Processor brand string part 2 */
		res.eax = 0x2E317620;
		res.ebx = 0x00000030;
		res.ecx = 0;
		res.edx = 0;
		break;

	case 0x80000004:
		/* Processor brand string part 3 */
		res.eax = 0;
		res.ebx = 0;
		res.ecx = 0;
		res.edx = 0;
		break;

	case 0x80000008:
		/* Virtual/physical address sizes: 48-bit virtual, 39-bit physical */
		res.eax = 0x00003027;
		break;

	default:
		/* Return zeros for unrecognised leaves */
		if (subleaf) {
			/* silence compiler warning */
		}
		break;
	}

	vcpu->regs.rax = res.eax;
	vcpu->regs.rbx = res.ebx;
	vcpu->regs.rcx = res.ecx;
	vcpu->regs.rdx = res.edx;
}

/* ---- MSR emulation ----------------------------------------------------- */

#define MSR_IA32_TSC                    0x00000010
#ifndef MSR_IA32_APICBASE
#define MSR_IA32_APICBASE               0x0000001B
#endif
#define MSR_IA32_SYSENTER_CS            0x00000174
#define MSR_IA32_SYSENTER_ESP           0x00000175
#define MSR_IA32_SYSENTER_EIP           0x00000176
#ifndef MSR_IA32_MISC_ENABLE
#define MSR_IA32_MISC_ENABLE            0x000001A0
#endif
#define MSR_IA32_PAT                    0x00000277
#define MSR_IA32_EFER                   0xC0000080
#define MSR_IA32_STAR                   0xC0000081
#define MSR_IA32_LSTAR                  0xC0000082
#define MSR_IA32_CSTAR                  0xC0000083
#define MSR_IA32_FMASK                  0xC0000084
#define MSR_IA32_FS_BASE                0xC0000100
#define MSR_IA32_GS_BASE                0xC0000101
#define MSR_IA32_KERNEL_GS_BASE         0xC0000102
#define MSR_IA32_TSC_AUX                0xC0000103

/* Per-VCPU MSR shadow storage — kept in a simple static array for now */
#define MSR_SHADOW_MAX                  16

struct msr_shadow {
	u32 index;
	u64 value;
};

static DEFINE_PER_CPU(struct msr_shadow[MSR_SHADOW_MAX], msr_shadows);
static DEFINE_PER_CPU(unsigned int, msr_shadow_count);

static u64 msr_shadow_read(u32 index)
{
	struct msr_shadow *shadows = this_cpu_ptr(msr_shadows);
	unsigned int count = this_cpu_read(msr_shadow_count);
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (shadows[i].index == index)
			return shadows[i].value;
	}

	/* Return safe defaults for known MSRs */
	switch (index) {
	case MSR_IA32_EFER:
		return (1ULL << 8) | (1ULL << 10); /* LME | LMA */
	case MSR_IA32_PAT:
		return 0x0007040600070406ULL; /* default PAT */
	case MSR_IA32_MISC_ENABLE:
		return (1ULL << 0); /* fast string enable */
	case MSR_IA32_APICBASE:
		return 0xFEE00900ULL; /* default APIC base, BSP */
	default:
		return 0;
	}
}

static void msr_shadow_write(u32 index, u64 value)
{
	struct msr_shadow *shadows = this_cpu_ptr(msr_shadows);
	unsigned int count = this_cpu_read(msr_shadow_count);
	unsigned int i;

	/* Update existing shadow */
	for (i = 0; i < count; i++) {
		if (shadows[i].index == index) {
			shadows[i].value = value;
			return;
		}
	}

	/* Add new shadow */
	if (count < MSR_SHADOW_MAX) {
		shadows[count].index = index;
		shadows[count].value = value;
		this_cpu_write(msr_shadow_count, count + 1);
	}
}

static int hv_handle_msr_read(struct hv_vcpu *vcpu)
{
	u32 msr = (u32)vcpu->regs.rcx;
	u64 value;

	value = msr_shadow_read(msr);
	vcpu->regs.rax = value & 0xFFFFFFFF;
	vcpu->regs.rdx = value >> 32;

	return 0;
}

static int hv_handle_msr_write(struct hv_vcpu *vcpu)
{
	u32 msr = (u32)vcpu->regs.rcx;
	u64 value = ((u64)(u32)vcpu->regs.rdx << 32) | (u32)vcpu->regs.rax;

	/* Validate critical MSR writes */
	switch (msr) {
	case MSR_IA32_EFER:
		/* Ensure LME and LMA stay consistent with long mode */
		if (!(value & (1ULL << 8))) {
			pr_warn("straylight-hv: guest tried to disable LME\n");
			return -EINVAL;
		}
		break;
	case MSR_IA32_APICBASE:
		/* Don't allow changing APIC base address */
		value = (value & 0xFFF) | 0xFEE00000ULL;
		break;
	}

	msr_shadow_write(msr, value);
	return 0;
}

/* ---- IO instruction emulation ------------------------------------------ */

static int hv_handle_io(struct hv_vcpu *vcpu, u64 exit_qual)
{
	u16 port;
	u8  size;
	bool is_in;
	bool is_string;

	/* Exit qualification encoding for I/O instructions:
	 * Bits 15:0  — port number
	 * Bits 2:0   — size (0=1B, 1=2B, 3=4B)
	 * Bit 3      — direction (0=OUT, 1=IN)
	 * Bit 4      — string instruction
	 * Bit 5      — REP prefix
	 * Bit 6      — operand encoding (imm/DX)
	 */
	port      = (u16)((exit_qual >> 16) & 0xFFFF);
	size      = (u8)((exit_qual & 0x7) + 1);
	is_in     = !!(exit_qual & (1ULL << 3));
	is_string = !!(exit_qual & (1ULL << 4));

	/*
	 * Emulate common I/O ports:
	 * 0x3F8-0x3FF: COM1 (serial)
	 * 0x60/0x64:   PS/2 keyboard controller
	 * 0x70/0x71:   CMOS/RTC
	 * 0xCF8/0xCFC: PCI configuration space
	 */

	if (is_in) {
		u32 value = 0;

		switch (port) {
		case 0x3F8 ... 0x3FF:
			/* COM1 — LSR: transmitter empty + idle */
			if (port == 0x3FD)
				value = 0x60;
			break;
		case 0x64:
			/* PS/2 status: output buffer empty */
			value = 0x00;
			break;
		case 0x71:
			/* CMOS data — return 0 */
			value = 0x00;
			break;
		case 0xCFC ... 0xCFF:
			/* PCI config data — return 0xFFFFFFFF (no device) */
			value = 0xFFFFFFFF;
			break;
		default:
			value = 0xFF;
			break;
		}

		vcpu->regs.rax = (vcpu->regs.rax & ~((1ULL << (size * 8)) - 1))
				 | (value & ((1ULL << (size * 8)) - 1));
	} else {
		/* OUT — we consume and discard for now */
		u32 value = (u32)(vcpu->regs.rax & ((1ULL << (size * 8)) - 1));

		switch (port) {
		case 0x3F8:
			/* COM1 TX — could log to kernel ring buffer */
			if (size == 1 && value >= 0x20 && value < 0x7F)
				pr_debug("straylight-hv: guest COM1: '%c'\n",
					 (char)value);
			break;
		default:
			break;
		}
	}

	if (is_string) {
		/* For string I/O, we'd need to handle RSI/RDI/RCX updates.
		 * In practice, string I/O is rare in modern guests. */
	}

	return 0;
}

/* ---- EPT violation handler --------------------------------------------- */

static int hv_handle_ept_violation(struct hv_vm_context *vm,
				   struct hv_vcpu *vcpu,
				   u64 exit_qual)
{
	u64 gpa;
	bool is_read, is_write, is_exec;

	/*
	 * Exit qualification bits:
	 * 0 = data read
	 * 1 = data write
	 * 2 = instruction fetch
	 * 3 = EPT R violation
	 * 4 = EPT W violation
	 * 5 = EPT X violation
	 */
	is_read  = !!(exit_qual & (1ULL << 0));
	is_write = !!(exit_qual & (1ULL << 1));
	is_exec  = !!(exit_qual & (1ULL << 2));

	/* Guest physical address is in the VMCS field */
	gpa = hv_vmcs_read64(VMCS_GUEST_PHYS_ADDR);

	pr_debug("straylight-hv: EPT violation at GPA 0x%llx "
		 "(R=%d W=%d X=%d)\n",
		 gpa, is_read, is_write, is_exec);

	/* Check if GPA is within guest memory range */
	if (gpa >= vm->mem_size) {
		pr_warn("straylight-hv: EPT violation outside guest memory: "
			"GPA=0x%llx (limit=0x%llx)\n", gpa, vm->mem_size);
		return -EFAULT;
	}

	/*
	 * The page might have been unmapped (e.g., after a balloon request)
	 * or never mapped.  Re-establish the mapping.
	 */
	{
		void *hva = vm->guest_mem + gpa;
		struct page *page = vmalloc_to_page(hva);
		u64 flags = EPT_PTE_READ;
		int ret;

		if (!page)
			return -EFAULT;

		if (is_write)
			flags |= EPT_PTE_WRITE;
		if (is_exec)
			flags |= EPT_PTE_EXEC;
		/* Default to full permissions for re-maps */
		flags |= EPT_PTE_WRITE | EPT_PTE_EXEC;

		ret = hv_ept_map_page(&vm->ept, gpa & PAGE_MASK,
				      page_to_phys(page), flags);
		if (ret) {
			pr_err("straylight-hv: failed to re-map EPT at "
			       "GPA 0x%llx\n", gpa);
			return ret;
		}
	}

	return 0;
}

/* ---- Top-level exit dispatch ------------------------------------------- */

int hv_handle_exit(struct hv_vm_context *vm, struct hv_vcpu *vcpu,
		   u32 exit_reason, u64 exit_qual)
{
	int ret = 0;
	u32 instr_len;

	switch (exit_reason) {
	case EXIT_REASON_CPUID:
		hv_emulate_cpuid(vcpu);
		/* Advance RIP past the CPUID instruction (2 bytes: 0F A2) */
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 2;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_MSR_READ:
		ret = hv_handle_msr_read(vcpu);
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 2;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_MSR_WRITE:
		ret = hv_handle_msr_write(vcpu);
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 2;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_IO_INSTRUCTION:
		ret = hv_handle_io(vcpu, exit_qual);
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 1;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_HLT:
		/* Guest executed HLT — yield time slice */
		pr_debug("straylight-hv: guest HLT on VCPU %u\n", vcpu->id);
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 1;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_EPT_VIOLATION:
		ret = hv_handle_ept_violation(vm, vcpu, exit_qual);
		/* EPT violation does not advance RIP */
		break;

	case EXIT_REASON_EPT_MISCONFIG:
		pr_err("straylight-hv: EPT misconfiguration at GPA 0x%llx\n",
		       hv_vmcs_read64(VMCS_GUEST_PHYS_ADDR));
		ret = -EIO;
		break;

	case EXIT_REASON_VMCALL:
		/* Hypercall interface — return StrayLight signature */
		vcpu->regs.rax = 0x5354524159ULL; /* "STRAY" */
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 3;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_CR_ACCESS:
		/* CR access — for now, just advance past the instruction */
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 3;
		vcpu->regs.rip += instr_len;
		break;

	case EXIT_REASON_TRIPLE_FAULT:
		pr_err("straylight-hv: triple fault on VCPU %u!\n", vcpu->id);
		ret = -EFAULT;
		break;

	case EXIT_REASON_EXTERNAL_INT:
		/* External interrupt — just re-enter the guest */
		break;

	case EXIT_REASON_XSETBV:
		/* XSETBV — advance RIP */
		instr_len = hv_vmcs_read32(VMCS_EXIT_INSTR_LEN);
		if (instr_len == 0)
			instr_len = 3;
		vcpu->regs.rip += instr_len;
		break;

	default:
		pr_warn("straylight-hv: unhandled exit reason %u "
			"(qual=0x%llx) on VCPU %u\n",
			exit_reason, exit_qual, vcpu->id);
		ret = -ENOSYS;
		break;
	}

	return ret;
}
