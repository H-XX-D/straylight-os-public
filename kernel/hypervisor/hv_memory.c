// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — EPT (Extended Page Tables) Management
 * Copyright (C) 2026 StrayLight Systems
 *
 * 4-level EPT page table: PML4 -> PDPT -> PD -> PT
 * Supports 4 KiB granularity mapping of guest physical addresses
 * to host physical addresses.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <asm/io.h>

#include "hv.h"

/* ---- EPT table index extraction ---------------------------------------- */

static inline unsigned int ept_pml4_index(u64 gpa)
{
	return (unsigned int)((gpa >> 39) & 0x1FF);
}

static inline unsigned int ept_pdpt_index(u64 gpa)
{
	return (unsigned int)((gpa >> 30) & 0x1FF);
}

static inline unsigned int ept_pd_index(u64 gpa)
{
	return (unsigned int)((gpa >> 21) & 0x1FF);
}

static inline unsigned int ept_pt_index(u64 gpa)
{
	return (unsigned int)((gpa >> 12) & 0x1FF);
}

/* ---- Page allocation helper -------------------------------------------- */

static u64 *ept_alloc_table(struct hv_ept_table *ept)
{
	struct page *page;
	u64 *table;

	if (ept->nr_pages >= HV_MAX_EPT_PAGES) {
		pr_err("straylight-hv: EPT page limit reached (%u)\n",
		       HV_MAX_EPT_PAGES);
		return NULL;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return NULL;

	ept->pages[ept->nr_pages++] = page;
	table = page_address(page);
	return table;
}

/* ---- EPT init / destroy ------------------------------------------------ */

int hv_ept_init(struct hv_ept_table *ept)
{
	mutex_init(&ept->lock);
	ept->nr_pages = 0;
	memset(ept->pages, 0, sizeof(ept->pages));

	ept->pml4 = ept_alloc_table(ept);
	if (!ept->pml4)
		return -ENOMEM;

	pr_debug("straylight-hv: EPT initialised (PML4 at %px)\n", ept->pml4);
	return 0;
}

void hv_ept_destroy(struct hv_ept_table *ept)
{
	unsigned int i;

	mutex_lock(&ept->lock);
	for (i = 0; i < ept->nr_pages; i++) {
		if (ept->pages[i]) {
			__free_page(ept->pages[i]);
			ept->pages[i] = NULL;
		}
	}
	ept->nr_pages = 0;
	ept->pml4 = NULL;
	mutex_unlock(&ept->lock);

	mutex_destroy(&ept->lock);
}

/* ---- Walk or allocate intermediate levels ------------------------------ */

static u64 *ept_get_or_create_table(struct hv_ept_table *ept,
				    u64 *parent, unsigned int index)
{
	u64 entry = parent[index];
	u64 *child;

	if (entry & EPT_PTE_READ) {
		/* Entry already present — extract physical address */
		unsigned long phys = entry & EPT_PTE_ADDR_MASK;
		child = phys_to_virt(phys);
		return child;
	}

	/* Allocate a new table */
	child = ept_alloc_table(ept);
	if (!child)
		return NULL;

	/* Install entry: R + W + X, pointing to the new child table */
	parent[index] = virt_to_phys(child) |
			EPT_PTE_READ | EPT_PTE_WRITE | EPT_PTE_EXEC;

	return child;
}

/* ---- Map a single 4 KiB page ------------------------------------------ */

int hv_ept_map_page(struct hv_ept_table *ept, u64 gpa, u64 hpa, u64 flags)
{
	u64 *pml4e, *pdpte, *pde, *pte;
	unsigned int idx;

	mutex_lock(&ept->lock);

	/* Level 4: PML4 */
	idx = ept_pml4_index(gpa);
	pml4e = ept_get_or_create_table(ept, ept->pml4, idx);
	if (!pml4e) {
		mutex_unlock(&ept->lock);
		return -ENOMEM;
	}

	/* Level 3: PDPT */
	idx = ept_pdpt_index(gpa);
	pdpte = ept_get_or_create_table(ept, pml4e, idx);
	if (!pdpte) {
		mutex_unlock(&ept->lock);
		return -ENOMEM;
	}

	/* Level 2: PD */
	idx = ept_pd_index(gpa);
	pde = ept_get_or_create_table(ept, pdpte, idx);
	if (!pde) {
		mutex_unlock(&ept->lock);
		return -ENOMEM;
	}

	/* Level 1: PT — install the leaf entry */
	idx = ept_pt_index(gpa);
	pte = &pde[idx];

	*pte = (hpa & EPT_PTE_ADDR_MASK) | flags |
	       ((u64)EPT_MEM_TYPE_WB << EPT_PTE_MEM_TYPE_SHIFT);

	mutex_unlock(&ept->lock);
	return 0;
}

/* ---- Unmap a single 4 KiB page ---------------------------------------- */

int hv_ept_unmap_page(struct hv_ept_table *ept, u64 gpa)
{
	u64 *table;
	u64 entry;
	unsigned long phys;
	unsigned int idx;

	mutex_lock(&ept->lock);

	/* Walk PML4 */
	idx = ept_pml4_index(gpa);
	entry = ept->pml4[idx];
	if (!(entry & EPT_PTE_READ))
		goto not_mapped;
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* Walk PDPT */
	idx = ept_pdpt_index(gpa);
	entry = table[idx];
	if (!(entry & EPT_PTE_READ))
		goto not_mapped;
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* Walk PD */
	idx = ept_pd_index(gpa);
	entry = table[idx];
	if (!(entry & EPT_PTE_READ))
		goto not_mapped;
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* Clear PT entry */
	idx = ept_pt_index(gpa);
	phys = table[idx] & EPT_PTE_ADDR_MASK;
	table[idx] = 0;

	mutex_unlock(&ept->lock);
	return 0;

not_mapped:
	mutex_unlock(&ept->lock);
	return -ENOENT;
}

/* ---- Translate GPA -> HPA via EPT walk --------------------------------- */

u64 hv_ept_translate(struct hv_ept_table *ept, u64 gpa)
{
	u64 *table;
	u64 entry;
	unsigned int idx;

	mutex_lock(&ept->lock);

	/* PML4 */
	idx = ept_pml4_index(gpa);
	entry = ept->pml4[idx];
	if (!(entry & EPT_PTE_READ))
		goto fault;
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* PDPT */
	idx = ept_pdpt_index(gpa);
	entry = table[idx];
	if (!(entry & EPT_PTE_READ))
		goto fault;
	/* Check for 1 GiB large page */
	if (entry & EPT_PTE_LARGE) {
		u64 hpa = (entry & EPT_PTE_ADDR_MASK) | (gpa & 0x3FFFFFFFUL);
		mutex_unlock(&ept->lock);
		return hpa;
	}
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* PD */
	idx = ept_pd_index(gpa);
	entry = table[idx];
	if (!(entry & EPT_PTE_READ))
		goto fault;
	/* Check for 2 MiB large page */
	if (entry & EPT_PTE_LARGE) {
		u64 hpa = (entry & EPT_PTE_ADDR_MASK) | (gpa & 0x1FFFFFUL);
		mutex_unlock(&ept->lock);
		return hpa;
	}
	table = phys_to_virt(entry & EPT_PTE_ADDR_MASK);

	/* PT */
	idx = ept_pt_index(gpa);
	entry = table[idx];
	if (!(entry & EPT_PTE_READ))
		goto fault;

	mutex_unlock(&ept->lock);
	return (entry & EPT_PTE_ADDR_MASK) | (gpa & 0xFFFUL);

fault:
	mutex_unlock(&ept->lock);
	return (u64)-1;
}
