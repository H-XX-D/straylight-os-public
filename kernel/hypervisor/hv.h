/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — KVM Extensions / Hypervisor
 * Copyright (C) 2026 StrayLight Systems
 *
 * VMCS field definitions, VM context, ioctl interface.
 */

#ifndef _STRAYLIGHT_HV_H
#define _STRAYLIGHT_HV_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

/* ---- VMCS field encodings (Intel SDM Vol. 3D, Appendix B) -------------- */

/* 16-bit control fields */
#define VMCS_VPID                       0x00000000
#define VMCS_POSTED_INT_NOTIFY          0x00000002

/* 16-bit guest-state fields */
#define VMCS_GUEST_ES_SEL               0x00000800
#define VMCS_GUEST_CS_SEL               0x00000802
#define VMCS_GUEST_SS_SEL               0x00000804
#define VMCS_GUEST_DS_SEL               0x00000806
#define VMCS_GUEST_FS_SEL               0x00000808
#define VMCS_GUEST_GS_SEL               0x0000080A
#define VMCS_GUEST_LDTR_SEL             0x0000080C
#define VMCS_GUEST_TR_SEL               0x0000080E

/* 32-bit control fields */
#define VMCS_PIN_BASED_CTLS             0x00004000
#define VMCS_PROC_BASED_CTLS            0x00004002
#define VMCS_EXCEPTION_BITMAP           0x00004004
#define VMCS_EXIT_CTLS                  0x0000400C
#define VMCS_ENTRY_CTLS                 0x00004012
#define VMCS_PROC_BASED_CTLS2           0x0000401E

/* 32-bit guest-state fields */
#define VMCS_GUEST_ES_LIMIT             0x00004800
#define VMCS_GUEST_CS_LIMIT             0x00004802
#define VMCS_GUEST_SS_LIMIT             0x00004804
#define VMCS_GUEST_DS_LIMIT             0x00004806
#define VMCS_GUEST_FS_LIMIT             0x00004808
#define VMCS_GUEST_GS_LIMIT             0x0000480A
#define VMCS_GUEST_LDTR_LIMIT           0x0000480C
#define VMCS_GUEST_TR_LIMIT             0x0000480E
#define VMCS_GUEST_GDTR_LIMIT           0x00004810
#define VMCS_GUEST_IDTR_LIMIT           0x00004812
#define VMCS_GUEST_ES_AR                0x00004814
#define VMCS_GUEST_CS_AR                0x00004816
#define VMCS_GUEST_SS_AR                0x00004818
#define VMCS_GUEST_DS_AR                0x0000481A
#define VMCS_GUEST_FS_AR                0x0000481C
#define VMCS_GUEST_GS_AR               0x0000481E
#define VMCS_GUEST_INTERRUPTIBILITY     0x00004824
#define VMCS_GUEST_ACTIVITY             0x00004826

/* 64-bit control fields */
#define VMCS_IO_BITMAP_A                0x00002000
#define VMCS_IO_BITMAP_B                0x00002002
#define VMCS_MSR_BITMAP                 0x00002004
#define VMCS_EXIT_MSR_STORE_ADDR        0x00002006
#define VMCS_EXIT_MSR_LOAD_ADDR         0x00002008
#define VMCS_ENTRY_MSR_LOAD_ADDR        0x0000200A
#define VMCS_EPT_PTR                    0x0000201A
#define VMCS_VMFUNC_CONTROLS            0x00002018

/* Natural-width guest-state */
#define VMCS_GUEST_CR0                  0x00006800
#define VMCS_GUEST_CR3                  0x00006802
#define VMCS_GUEST_CR4                  0x00006804
#define VMCS_GUEST_ES_BASE              0x00006806
#define VMCS_GUEST_CS_BASE              0x00006808
#define VMCS_GUEST_SS_BASE              0x0000680A
#define VMCS_GUEST_DS_BASE              0x0000680C
#define VMCS_GUEST_FS_BASE              0x0000680E
#define VMCS_GUEST_GS_BASE              0x00006810
#define VMCS_GUEST_LDTR_BASE            0x00006812
#define VMCS_GUEST_TR_BASE              0x00006814
#define VMCS_GUEST_GDTR_BASE            0x00006816
#define VMCS_GUEST_IDTR_BASE            0x00006818
#define VMCS_GUEST_DR7                  0x0000681A
#define VMCS_GUEST_RSP                  0x0000681C
#define VMCS_GUEST_RIP                  0x0000681E
#define VMCS_GUEST_RFLAGS               0x00006820

/* Natural-width host-state */
#define VMCS_HOST_CR0                   0x00006C00
#define VMCS_HOST_CR3                   0x00006C02
#define VMCS_HOST_CR4                   0x00006C04
#define VMCS_HOST_RSP                   0x00006C14
#define VMCS_HOST_RIP                   0x00006C16

/* VM-exit reason field */
#define VMCS_EXIT_REASON                0x00004402
#define VMCS_EXIT_QUALIFICATION         0x00006400
#define VMCS_EXIT_INSTR_LEN             0x0000440C
#define VMCS_EXIT_INSTR_INFO            0x0000440E
#define VMCS_GUEST_PHYS_ADDR            0x00002400

/* ---- VM-exit reasons --------------------------------------------------- */
#define EXIT_REASON_EXCEPTION_NMI       0
#define EXIT_REASON_EXTERNAL_INT        1
#define EXIT_REASON_TRIPLE_FAULT        2
#define EXIT_REASON_CPUID               10
#define EXIT_REASON_HLT                 12
#define EXIT_REASON_INVLPG              14
#define EXIT_REASON_RDPMC               15
#define EXIT_REASON_RDTSC               16
#define EXIT_REASON_VMCALL              18
#define EXIT_REASON_CR_ACCESS           28
#define EXIT_REASON_IO_INSTRUCTION      30
#define EXIT_REASON_MSR_READ            31
#define EXIT_REASON_MSR_WRITE           32
#define EXIT_REASON_EPT_VIOLATION       48
#define EXIT_REASON_EPT_MISCONFIG       49
#define EXIT_REASON_XSETBV              55

/* ---- EPT definitions --------------------------------------------------- */
#define EPT_LEVELS                      4
#define EPT_PAGE_WALK_LENGTH            (EPT_LEVELS - 1)
#define EPT_ENTRIES_PER_TABLE           512
#define EPT_MEM_TYPE_WB                 6
#define EPT_PTE_READ                    (1ULL << 0)
#define EPT_PTE_WRITE                   (1ULL << 1)
#define EPT_PTE_EXEC                    (1ULL << 2)
#define EPT_PTE_MEM_TYPE_SHIFT          3
#define EPT_PTE_LARGE                   (1ULL << 7)
#define EPT_PTE_ADDR_MASK               0x000FFFFFFFFFF000ULL

#define HV_MAX_EPT_PAGES               4096

/* ---- ioctl interface --------------------------------------------------- */
#define HV_IOC_MAGIC                    'H'

#define HV_IOC_CREATE_VM        _IOWR(HV_IOC_MAGIC, 0x01, struct hv_create_vm_req)
#define HV_IOC_DESTROY_VM       _IOW (HV_IOC_MAGIC, 0x02, struct hv_destroy_vm_req)
#define HV_IOC_RUN_VM           _IOWR(HV_IOC_MAGIC, 0x03, struct hv_run_vm_req)
#define HV_IOC_SET_REGS         _IOW (HV_IOC_MAGIC, 0x04, struct hv_regs_req)
#define HV_IOC_GET_REGS         _IOWR(HV_IOC_MAGIC, 0x05, struct hv_regs_req)
#define HV_IOC_MAP_MEMORY       _IOW (HV_IOC_MAGIC, 0x06, struct hv_map_mem_req)
#define HV_IOC_GET_STATS        _IOWR(HV_IOC_MAGIC, 0x07, struct hv_stats_req)

struct hv_create_vm_req {
	__u32 vm_id;            /* out: assigned VM ID */
	__u32 nr_vcpus;         /* in: number of VCPUs */
	__u64 mem_size;         /* in: guest physical memory size */
};

struct hv_destroy_vm_req {
	__u32 vm_id;
	__u32 pad;
};

struct hv_run_vm_req {
	__u32 vm_id;
	__u32 exit_reason;      /* out */
	__u64 exit_qualification; /* out */
};

struct hv_regs_req {
	__u32 vm_id;
	__u32 vcpu_id;
	__u64 rax, rbx, rcx, rdx;
	__u64 rsi, rdi, rbp, rsp;
	__u64 r8, r9, r10, r11;
	__u64 r12, r13, r14, r15;
	__u64 rip, rflags;
	__u64 cr0, cr3, cr4;
};

struct hv_map_mem_req {
	__u32 vm_id;
	__u32 flags;
	__u64 gpa;              /* guest physical address */
	__u64 hva;              /* host virtual address */
	__u64 size;
};

struct hv_stats_req {
	__u32 vm_id;
	__u32 pad;
	__u64 cpuid_exits;
	__u64 msr_exits;
	__u64 io_exits;
	__u64 ept_violations;
	__u64 total_exits;
	__u64 total_run_ns;
};

/* ---- EPT page table ---------------------------------------------------- */
struct hv_ept_table {
	__u64                   *pml4;          /* PML4 page */
	struct page             *pages[HV_MAX_EPT_PAGES];
	unsigned int            nr_pages;
	struct mutex            lock;
};

/* ---- Per-VCPU state ---------------------------------------------------- */
struct hv_vcpu {
	unsigned int            id;
	struct page             *vmcs_page;
	unsigned long           vmcs_phys;
	struct hv_regs_req      regs;           /* shadow register state */
	bool                    launched;
};

/* ---- Per-VM profiling counters ----------------------------------------- */
struct hv_profile {
	atomic64_t              cpuid_exits;
	atomic64_t              msr_exits;
	atomic64_t              io_exits;
	atomic64_t              ept_violations;
	atomic64_t              other_exits;
	atomic64_t              total_exits;
	atomic64_t              total_run_cycles;
};

/* ---- Per-VM context ---------------------------------------------------- */
#define HV_MAX_VCPUS           64

struct hv_vm_context {
	struct list_head        list;
	__u32                   vm_id;
	unsigned int            nr_vcpus;
	struct hv_vcpu          vcpus[HV_MAX_VCPUS];
	struct hv_ept_table     ept;
	struct hv_profile       profile;
	__u64                   mem_size;
	void                    *guest_mem;     /* guest phys backing */
	struct mutex            lock;
};

/* ---- Global hypervisor state ------------------------------------------- */
struct hv_device {
	struct miscdevice       miscdev;
	struct list_head        vm_list;
	struct mutex            vm_lock;
	__u32                   next_vm_id;
	bool                    vmx_enabled;
	struct proc_dir_entry   *proc_dir;
};

/* ---- sub-module API ---------------------------------------------------- */

/* hv_vmcs.c */
int  hv_vmcs_alloc(struct hv_vcpu *vcpu);
void hv_vmcs_free(struct hv_vcpu *vcpu);
int  hv_vmcs_setup(struct hv_vcpu *vcpu, struct hv_vm_context *vm);
u32  hv_vmcs_read32(unsigned long field);
u64  hv_vmcs_read64(unsigned long field);
void hv_vmcs_write32(unsigned long field, u32 value);
void hv_vmcs_write64(unsigned long field, u64 value);

/* hv_memory.c */
int  hv_ept_init(struct hv_ept_table *ept);
void hv_ept_destroy(struct hv_ept_table *ept);
int  hv_ept_map_page(struct hv_ept_table *ept, u64 gpa, u64 hpa, u64 flags);
int  hv_ept_unmap_page(struct hv_ept_table *ept, u64 gpa);
u64  hv_ept_translate(struct hv_ept_table *ept, u64 gpa);

/* hv_intercept.c */
int  hv_handle_exit(struct hv_vm_context *vm, struct hv_vcpu *vcpu,
		    u32 exit_reason, u64 exit_qual);

/* hv_profiler.c */
void hv_profile_init(struct hv_profile *prof);
void hv_profile_record_exit(struct hv_profile *prof, u32 reason, u64 cycles);
int  hv_profiler_proc_init(struct hv_device *hdev);
void hv_profiler_proc_cleanup(struct hv_device *hdev);

#endif /* _STRAYLIGHT_HV_H */
