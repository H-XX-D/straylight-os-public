// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — CPU Timing Jitter Entropy
 * Copyright (C) 2026 StrayLight Systems
 *
 * Collects entropy from CPU timing jitter by measuring rdtsc delta
 * variance across repeated memory and arithmetic operations.
 *
 * Based on the principles from Stephan Mueller's jitterentropy library
 * (https://www.chronox.de/jent.html) adapted for in-kernel use.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/string.h>
#include <asm/tsc.h>

#include "entropy.h"

/* ---- Configuration ----------------------------------------------------- */

/*
 * Number of loop iterations for each entropy collection round.
 * More iterations = more jitter samples = better entropy per byte,
 * but higher latency.
 */
#define JITTER_LOOPS            64
#define JITTER_MEM_BLOCKS       512
#define JITTER_MEM_BLOCK_SIZE   32
#define JITTER_FOLD_LOOPS       128

/* ---- Jitter collector state -------------------------------------------- */

struct jitter_state {
	u64             prev_time;
	u64             last_delta;
	u64             last_delta2;
	u64             entropy_pool;
	u8              *mem;
	size_t          mem_size;
	unsigned int    stuck_count;
	spinlock_t      lock;
};

static struct jitter_state *g_jitter;

/* ---- rdtsc wrapper ----------------------------------------------------- */

static __always_inline u64 jitter_rdtsc(void)
{
#ifdef CONFIG_X86_64
	u32 lo, hi;

	/*
	 * Use RDTSC with a serialising fence.
	 * LFENCE ensures we read the TSC after prior instructions complete,
	 * providing more consistent jitter measurements.
	 */
	asm volatile("lfence; rdtsc" : "=a" (lo), "=d" (hi));
	return ((u64)hi << 32) | lo;
#else
	return get_cycles();
#endif
}

/* ---- Memory access pattern --------------------------------------------- */

/*
 * Perform memory operations that create timing variance due to:
 * - Cache line evictions
 * - TLB misses
 * - Memory controller contention
 * - DRAM refresh timing
 */
static void jitter_memaccess(struct jitter_state *js)
{
	size_t i;
	u8 *mem = js->mem;
	size_t wrap = js->mem_size - JITTER_MEM_BLOCK_SIZE;

	for (i = 0; i < JITTER_MEM_BLOCKS; i++) {
		size_t offset = (i * JITTER_MEM_BLOCK_SIZE * 7) % wrap;
		u64 *p = (u64 *)(mem + offset);

		/*
		 * Mixed read-modify-write pattern to defeat
		 * prefetcher predictions.
		 */
		p[0] ^= p[1];
		p[1] += p[2];
		p[2] = (p[2] << 17) | (p[2] >> 47);
		p[3] ^= p[0] + i;
	}
}

/* ---- Folding operation ------------------------------------------------- */

/*
 * Fold timing deltas into the entropy pool using a simple
 * non-linear mixing function. Each iteration adds one bit
 * of entropy (conservatively estimated).
 */
static void jitter_fold(struct jitter_state *js, u64 delta)
{
	unsigned int i;
	u64 fold = delta;

	for (i = 0; i < JITTER_FOLD_LOOPS; i++) {
		fold = fold ^ (fold >> 4);
		fold = fold * 0x9E3779B97F4A7C15ULL; /* golden ratio */
		fold = fold ^ (fold >> 17);
		fold = fold * 0xBF58476D1CE4E5B9ULL;
		fold = fold ^ (fold >> 31);
	}

	js->entropy_pool ^= fold;
	js->entropy_pool = (js->entropy_pool << 7) |
			   (js->entropy_pool >> 57);
}

/* ---- Stuck detector ---------------------------------------------------- */

/*
 * Detect if the TSC delta shows no variation (stuck CPU timer).
 * If the delta, first derivative, and second derivative are all zero,
 * the source is stuck and provides no entropy.
 */
static bool jitter_stuck(struct jitter_state *js, u64 delta)
{
	u64 delta2 = delta - js->last_delta;
	u64 delta3 = delta2 - js->last_delta2;

	js->last_delta2 = delta2;
	js->last_delta  = delta;

	if (delta == 0 && delta2 == 0 && delta3 == 0) {
		js->stuck_count++;
		return true;
	}

	js->stuck_count = 0;
	return false;
}

/* ---- Collect one round of entropy -------------------------------------- */

static int jitter_collect_round(struct jitter_state *js)
{
	u64 time_before, time_after, delta;
	unsigned int i;
	unsigned int stuck_in_row = 0;

	for (i = 0; i < JITTER_LOOPS; i++) {
		time_before = jitter_rdtsc();

		/* CPU-bound operations to generate jitter */
		jitter_memaccess(js);

		time_after = jitter_rdtsc();

		/* Ensure time moved forward */
		if (time_after <= time_before) {
			stuck_in_row++;
			if (stuck_in_row > 10)
				return -EIO;
			continue;
		}
		stuck_in_row = 0;

		delta = time_after - time_before;

		/* Check for stuck condition */
		if (jitter_stuck(js, delta))
			continue;

		/* Fold delta into entropy pool */
		jitter_fold(js, delta);
	}

	return 0;
}

/* ---- Public API -------------------------------------------------------- */

int sl_jitter_init(void)
{
	struct jitter_state *js;
	u64 time1, time2;
	int i, ret;

	/* Verify TSC is functional */
	time1 = jitter_rdtsc();
	for (i = 0; i < 100; i++)
		cpu_relax();
	time2 = jitter_rdtsc();

	if (time2 <= time1) {
		pr_warn("straylight-entropy: TSC not advancing, "
			"jitter source unavailable\n");
		return -ENODEV;
	}

	js = kzalloc(sizeof(*js), GFP_KERNEL);
	if (!js)
		return -ENOMEM;

	/* Allocate memory buffer for jitter generation.
	 * Use kmalloc (not vmalloc) to get physically contiguous memory
	 * for more cache-line-level jitter. */
	js->mem_size = JITTER_MEM_BLOCKS * JITTER_MEM_BLOCK_SIZE;
	js->mem = kmalloc(js->mem_size, GFP_KERNEL);
	if (!js->mem) {
		kfree(js);
		return -ENOMEM;
	}

	/* Initialise with non-zero pattern */
	memset(js->mem, 0xA5, js->mem_size);

	spin_lock_init(&js->lock);
	js->prev_time    = jitter_rdtsc();
	js->last_delta   = 0;
	js->last_delta2  = 0;
	js->entropy_pool = jitter_rdtsc();
	js->stuck_count  = 0;

	/* Warm up: run a few rounds to fill the entropy pool */
	for (i = 0; i < 4; i++) {
		ret = jitter_collect_round(js);
		if (ret) {
			kfree(js->mem);
			kfree(js);
			return ret;
		}
	}

	g_jitter = js;

	pr_info("straylight-entropy: jitter source initialised "
		"(mem=%zu bytes, loops=%d)\n",
		js->mem_size, JITTER_LOOPS);
	return 0;
}

void sl_jitter_cleanup(void)
{
	if (!g_jitter)
		return;

	memzero_explicit(g_jitter->mem, g_jitter->mem_size);
	kfree(g_jitter->mem);
	memzero_explicit(g_jitter, sizeof(*g_jitter));
	kfree(g_jitter);
	g_jitter = NULL;
}

int sl_jitter_read(void *buf, size_t count)
{
	struct jitter_state *js = g_jitter;
	u8 *out = buf;
	size_t generated = 0;
	unsigned long flags;
	int ret;

	if (!js)
		return -ENODEV;

	spin_lock_irqsave(&js->lock, flags);

	while (generated < count) {
		size_t chunk = min_t(size_t, count - generated, sizeof(u64));

		ret = jitter_collect_round(js);
		if (ret) {
			spin_unlock_irqrestore(&js->lock, flags);
			return generated > 0 ? (int)generated : ret;
		}

		/* Extract bytes from the entropy pool */
		memcpy(out + generated, &js->entropy_pool, chunk);
		generated += chunk;

		/* Re-seed the pool to avoid repeating output */
		js->entropy_pool ^= jitter_rdtsc();
	}

	spin_unlock_irqrestore(&js->lock, flags);
	return (int)generated;
}
