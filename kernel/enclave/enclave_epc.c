// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — EPC (Enclave Page Cache) Allocator
 * Copyright (C) 2026 StrayLight Systems
 *
 * Manages EPC page allocation and provides ECREATE/EADD/EINIT wrappers.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/bitmap.h>
#include <linux/string.h>
#include <asm/io.h>

#include "enclave.h"

/* ---- EPC page pool ----------------------------------------------------- */

#define EPC_POOL_SIZE           8192

struct epc_pool {
	struct mutex    lock;
	DECLARE_BITMAP(bitmap, EPC_POOL_SIZE);
	struct page     *pages[EPC_POOL_SIZE];
	unsigned int    nr_allocated;
	unsigned int    nr_total;
};

static struct epc_pool *g_epc_pool;

/* ---- SECS structure (SGX spec, Table 38-7) ----------------------------- */

struct sgx_secs {
	u64     size;
	u64     base_addr;
	u32     ssa_frame_size;
	u32     misc_select;
	u8      reserved1[24];
	u64     attributes;
	u64     xfrm;
	u32     mr_enclave[8];
	u8      reserved2[32];
	u32     mr_signer[8];
	u8      reserved3[96];
	u16     isv_prod_id;
	u16     isv_svn;
	u8      reserved4[3836];
} __packed;

#define SGX_ATTR_INIT           (1ULL << 0)
#define SGX_ATTR_DEBUG          (1ULL << 1)
#define SGX_ATTR_MODE64BIT      (1ULL << 2)

/* ---- ENCLS wrapper ----------------------------------------------------- */

#ifdef CONFIG_X86_64
static inline int __encls(u32 leaf, u64 rbx, u64 rcx, u64 rdx)
{
	int ret;

	asm volatile(
		"1: .byte 0x0F, 0x01, 0xCF\n"
		"   xor %%eax, %%eax\n"
		"2:\n"
		".section .fixup,\"ax\"\n"
		"3: mov $-1, %%eax\n"
		"   jmp 2b\n"
		".previous\n"
		"   .pushsection __ex_table, \"a\"\n"
		"   .balign 8\n"
		"   .quad 1b, 3b\n"
		"   .popsection\n"
		: "=a" (ret)
		: "a" (leaf), "b" (rbx), "c" (rcx), "d" (rdx)
		: "memory", "cc");
	return ret;
}
#endif

/* ---- Init / Cleanup ---------------------------------------------------- */

int sl_epc_init(struct sl_sgx_device *dev)
{
	unsigned int i;

	g_epc_pool = kzalloc(sizeof(*g_epc_pool), GFP_KERNEL);
	if (!g_epc_pool)
		return -ENOMEM;

	mutex_init(&g_epc_pool->lock);
	bitmap_zero(g_epc_pool->bitmap, EPC_POOL_SIZE);
	g_epc_pool->nr_allocated = 0;
	g_epc_pool->nr_total = EPC_POOL_SIZE;

	for (i = 0; i < EPC_POOL_SIZE; i++) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (!page) {
			g_epc_pool->nr_total = i;
			break;
		}
		g_epc_pool->pages[i] = page;
	}

	pr_info("straylight-enclave: EPC pool: %u pages (%lu MiB)\n",
		g_epc_pool->nr_total,
		(unsigned long)g_epc_pool->nr_total * PAGE_SIZE / (1024 * 1024));
	return 0;
}

void sl_epc_cleanup(struct sl_sgx_device *dev)
{
	unsigned int i;

	if (!g_epc_pool)
		return;

	for (i = 0; i < g_epc_pool->nr_total; i++) {
		if (g_epc_pool->pages[i])
			__free_page(g_epc_pool->pages[i]);
	}
	kfree(g_epc_pool);
	g_epc_pool = NULL;
}

/* ---- Page allocation --------------------------------------------------- */

int sl_epc_alloc_page(struct sl_sgx_device *dev, struct page **page_out,
		      u64 *phys_out)
{
	unsigned long idx;

	if (!g_epc_pool)
		return -ENODEV;

	mutex_lock(&g_epc_pool->lock);
	idx = find_first_zero_bit(g_epc_pool->bitmap, g_epc_pool->nr_total);
	if (idx >= g_epc_pool->nr_total) {
		mutex_unlock(&g_epc_pool->lock);
		return -ENOMEM;
	}
	__set_bit(idx, g_epc_pool->bitmap);
	g_epc_pool->nr_allocated++;
	mutex_unlock(&g_epc_pool->lock);

	*page_out = g_epc_pool->pages[idx];
	*phys_out = page_to_phys(g_epc_pool->pages[idx]);
	return 0;
}

void sl_epc_free_page(struct sl_sgx_device *dev, struct page *page)
{
	unsigned int i;

	if (!g_epc_pool || !page)
		return;

	mutex_lock(&g_epc_pool->lock);
	for (i = 0; i < g_epc_pool->nr_total; i++) {
		if (g_epc_pool->pages[i] == page) {
			clear_highpage(page);
			__clear_bit(i, g_epc_pool->bitmap);
			g_epc_pool->nr_allocated--;
			break;
		}
	}
	mutex_unlock(&g_epc_pool->lock);
}

/* ---- ECREATE ----------------------------------------------------------- */

int sl_epc_ecreate(struct sl_sgx_enclave *enc)
{
	struct sgx_secs *secs;
	void *secs_va;

	if (!enc->secs_page)
		return -EINVAL;

	secs_va = page_address(enc->secs_page);
	secs = (struct sgx_secs *)secs_va;

	memset(secs, 0, PAGE_SIZE);
	secs->size           = enc->size;
	secs->base_addr      = enc->base_addr;
	secs->ssa_frame_size = 1;
	secs->attributes     = SGX_ATTR_MODE64BIT | SGX_ATTR_DEBUG;
	secs->xfrm           = 0x03; /* x87 + SSE */

	/* Seed MRENCLAVE with ECREATE parameters */
	{
		u8 seed[64];

		memset(seed, 0, sizeof(seed));
		seed[0] = 0x00; /* ECREATE opcode */
		memcpy(seed + 8, &secs->size, 8);
		memcpy(seed + 16, &secs->ssa_frame_size, 4);
		memcpy(secs->mr_enclave, seed, 32);
	}

	pr_debug("straylight-enclave: ECREATE base=0x%llx size=%llu\n",
		 enc->base_addr, enc->size);
	return 0;
}

/* ---- EADD -------------------------------------------------------------- */

int sl_epc_eadd(struct sl_sgx_enclave *enc, u64 offset, void *src,
		u32 page_type, u64 flags)
{
	struct page *epc_page;
	u64 epc_phys;
	void *epc_va;
	struct sgx_secs *secs;
	int ret;

	if (enc->nr_pages >= SL_SGX_MAX_PAGES)
		return -ENOSPC;

	ret = sl_epc_alloc_page(NULL, &epc_page, &epc_phys);
	if (ret)
		return ret;

	epc_va = page_address(epc_page);
	memcpy(epc_va, src, PAGE_SIZE);

	/* Update MRENCLAVE measurement */
	secs = page_address(enc->secs_page);
	secs->mr_enclave[enc->nr_pages % 8] ^= (u32)(offset >> 12);
	secs->mr_enclave[(enc->nr_pages + 1) % 8] ^= ((u32 *)epc_va)[0];

	enc->epc_pages[enc->nr_pages] = epc_page;
	enc->epc_phys[enc->nr_pages]  = epc_phys;
	enc->nr_pages++;

	pr_debug("straylight-enclave: EADD page %u offset=0x%llx type=%u\n",
		 enc->nr_pages - 1, offset, page_type);
	return 0;
}

/* ---- EINIT ------------------------------------------------------------- */

int sl_epc_einit(struct sl_sgx_enclave *enc, void *sigstruct, void *token)
{
	struct sgx_secs *secs;

	if (!enc->secs_page)
		return -EINVAL;

	secs = page_address(enc->secs_page);

	/* Extract MRSIGNER from SIGSTRUCT */
	if (sigstruct)
		memcpy(secs->mr_signer, (u8 *)sigstruct + 128, 32);

	secs->attributes |= SGX_ATTR_INIT;

	/* Derive per-enclave sealing key from MRENCLAVE */
	{
		u32 i;
		u8 km[32];

		memcpy(km, secs->mr_enclave, 32);
		for (i = 0; i < 16; i++)
			enc->seal_key[i] = km[i] ^ km[i + 16];
		enc->seal_key_valid = true;
		memzero_explicit(km, sizeof(km));
	}

	pr_info("straylight-enclave: EINIT enclave %u (%u pages)\n",
		enc->id, enc->nr_pages);
	return 0;
}
