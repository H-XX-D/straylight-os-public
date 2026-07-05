/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — SGX Enclave Module, Shared Declarations
 * Copyright (C) 2026 StrayLight Systems
 *
 * Types and function declarations shared across all enclave sub-modules.
 */

#ifndef _STRAYLIGHT_ENCLAVE_H
#define _STRAYLIGHT_ENCLAVE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/mm_types.h>
#include <linux/miscdevice.h>

/* ---- SGX ENCLS leaf opcodes ------------------------------------------- */

#define SGX_ECREATE     0x00
#define SGX_EADD        0x01
#define SGX_EINIT       0x02
#define SGX_EREMOVE     0x03
#define SGX_EEXTEND     0x06

/* ---- SGX page types ---------------------------------------------------- */

#define SGX_PT_SECS     0
#define SGX_PT_TCS      1
#define SGX_PT_REG      2
#define SGX_PT_VA       3
#define SGX_PT_TRIM     4

/* ---- SECINFO flags ----------------------------------------------------- */

#define SGX_SECINFO_R           (1ULL << 0)
#define SGX_SECINFO_W           (1ULL << 1)
#define SGX_SECINFO_X           (1ULL << 2)
#define SGX_SECINFO_PT_REG      ((u64)SGX_PT_REG  << 8)
#define SGX_SECINFO_PT_TCS      ((u64)SGX_PT_TCS  << 8)
#define SGX_SECINFO_PT_TRIM     ((u64)SGX_PT_TRIM << 8)

/* ---- EGETKEY key names ------------------------------------------------- */

#define SGX_KEYNAME_SEAL        0x0004

/* ---- Sealed blob overhead --------------------------------------------- */

#define SL_SEAL_OVERHEAD        48U  /* header(16) + IV(16) + MAC(16) */
#define SL_SEAL_MAX_DATA        (1U << 20)

/* ---- Per-enclave context ----------------------------------------------- */

#define SL_SGX_MAX_PAGES        4096

struct sl_sgx_enclave {
	struct list_head        list;
	__u32                   id;
	__u64                   size;
	__u64                   base_addr;
	bool                    initialized;
	unsigned int            nr_pages;

	struct page             *epc_pages[SL_SGX_MAX_PAGES];
	u64                     epc_phys[SL_SGX_MAX_PAGES];

	struct page             *secs_page;
	u64                     secs_phys;

	u8                      seal_key[16];
	bool                    seal_key_valid;

	struct mutex            lock;
};

/* ---- Global device state ----------------------------------------------- */

struct sl_sgx_device {
	struct miscdevice       miscdev;
	struct list_head        enclave_list;
	struct mutex            list_lock;
	__u32                   next_id;
	bool                    sgx1_supported;
	bool                    sgx2_supported;
	u64                     epc_base;
	u64                     epc_size;
};

/* ---- Function declarations — enclave_epc.c ---------------------------- */

int   sl_epc_init(struct sl_sgx_device *dev);
void  sl_epc_cleanup(struct sl_sgx_device *dev);
int   sl_epc_alloc_page(struct sl_sgx_device *dev,
			struct page **page_out, u64 *phys_out);
void  sl_epc_free_page(struct sl_sgx_device *dev, struct page *page);
int   sl_epc_ecreate(struct sl_sgx_enclave *enc);
int   sl_epc_eadd(struct sl_sgx_enclave *enc, u64 offset, void *src,
		  u32 page_type, u64 flags);
int   sl_epc_einit(struct sl_sgx_enclave *enc,
		   void *sigstruct, void *token);

/* ---- Function declarations — enclave_sealed.c ------------------------- */

int   sl_seal_data(struct sl_sgx_enclave *enc,
		   const void *plain, size_t plain_len,
		   void *sealed, size_t *sealed_len);
int   sl_unseal_data(struct sl_sgx_enclave *enc,
		     const void *sealed, size_t sealed_len,
		     void *plain, size_t *plain_len);

/* ---- Function declarations — enclave_attestation.c -------------------- */

int   sl_generate_report(struct sl_sgx_enclave *enc,
			 const void *target_info,
			 const void *report_data,
			 void *report_out);
int   sl_verify_report(const void *report, size_t report_len);

#endif /* _STRAYLIGHT_ENCLAVE_H */
