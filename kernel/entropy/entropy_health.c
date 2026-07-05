// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Entropy Health Tests (NIST SP 800-90B)
 * Copyright (C) 2026 StrayLight Systems
 *
 * Implements the two mandatory health tests from NIST SP 800-90B
 * Section 4.4 for online entropy source validation:
 *
 * 1. Repetition Count Test — detects a single value repeating too many
 *    times consecutively.
 * 2. Adaptive Proportion Test — detects a value occurring too frequently
 *    within a sliding window.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

#include "entropy.h"

/* ---- Constants from NIST SP 800-90B ------------------------------------ */

/*
 * Parameters for 8-bit samples (noise source outputs bytes).
 *
 * H = min-entropy estimate.  We conservatively assume H = 4 bits/byte
 * (50% of maximum) which is appropriate for a hardware source mixing
 * multiple inputs.
 *
 * Repetition Count cutoff C:
 *   C = 1 + ceil(-log2(alpha) / H)
 * With alpha = 2^-20 (false positive rate) and H = 4:
 *   C = 1 + ceil(20/4) = 6
 */
#define REP_COUNT_CUTOFF        6

/*
 * Adaptive Proportion Test parameters:
 * Window size W = 1024 (binary noise source section recommends 512-1024)
 * Cutoff threshold depends on H and alpha.
 * For H=4 and alpha=2^-20 with W=1024:
 *   Expected count = W * 2^(-H) = 1024/16 = 64
 *   Cutoff ~ 64 + 6*sqrt(64*(1-1/16)) ≈ 64 + 46 = 110
 *   We use a slightly conservative cutoff of 100.
 */
#define ADAPT_WINDOW_SIZE       1024
#define ADAPT_CUTOFF            100

/* ---- Health test state ------------------------------------------------- */

struct health_state {
	/* Repetition Count Test */
	u8              rep_last_sample;
	unsigned int    rep_count;

	/* Adaptive Proportion Test */
	u8              apt_base;               /* value being counted  */
	unsigned int    apt_count;              /* occurrences of base  */
	unsigned int    apt_window_pos;         /* position in window   */

	/* Statistics */
	atomic64_t      total_samples;
	atomic64_t      failed_samples;
	atomic64_t      rep_failures;
	atomic64_t      prop_failures;

	spinlock_t      lock;
};

static struct health_state *g_health;

/* ---- Repetition Count Test --------------------------------------------- */

/*
 * NIST SP 800-90B, Section 4.4.1.
 *
 * Track consecutive repetitions of the same sample value.
 * If the count reaches REP_COUNT_CUTOFF, the test fails,
 * indicating the source may be stuck or malfunctioning.
 *
 * Returns: 0 if passed, -1 if failed.
 */
static int repetition_count_test(struct health_state *hs, u8 sample)
{
	if (sample == hs->rep_last_sample) {
		hs->rep_count++;
		if (hs->rep_count >= REP_COUNT_CUTOFF) {
			atomic64_inc(&hs->rep_failures);
			return -1;
		}
	} else {
		hs->rep_last_sample = sample;
		hs->rep_count = 1;
	}

	return 0;
}

/* ---- Adaptive Proportion Test ------------------------------------------ */

/*
 * NIST SP 800-90B, Section 4.4.2.
 *
 * Count how often the first sample in a window of W samples appears.
 * If it appears more than ADAPT_CUTOFF times, the test fails.
 *
 * Returns: 0 if passed, -1 if failed.
 */
static int adaptive_proportion_test(struct health_state *hs, u8 sample)
{
	if (hs->apt_window_pos == 0) {
		/* Start of a new window: set the base value */
		hs->apt_base       = sample;
		hs->apt_count      = 1;
		hs->apt_window_pos = 1;
		return 0;
	}

	hs->apt_window_pos++;

	if (sample == hs->apt_base)
		hs->apt_count++;

	/* Check cutoff within the window */
	if (hs->apt_count >= ADAPT_CUTOFF) {
		atomic64_inc(&hs->prop_failures);
		/* Reset window to begin fresh */
		hs->apt_window_pos = 0;
		return -1;
	}

	/* End of window — reset */
	if (hs->apt_window_pos >= ADAPT_WINDOW_SIZE)
		hs->apt_window_pos = 0;

	return 0;
}

/* ---- Public API -------------------------------------------------------- */

int sl_health_init(void)
{
	g_health = kzalloc(sizeof(*g_health), GFP_KERNEL);
	if (!g_health)
		return -ENOMEM;

	spin_lock_init(&g_health->lock);
	g_health->rep_last_sample = 0;
	g_health->rep_count       = 0;
	g_health->apt_base        = 0;
	g_health->apt_count       = 0;
	g_health->apt_window_pos  = 0;

	atomic64_set(&g_health->total_samples, 0);
	atomic64_set(&g_health->failed_samples, 0);
	atomic64_set(&g_health->rep_failures, 0);
	atomic64_set(&g_health->prop_failures, 0);

	pr_info("straylight-entropy: health tests initialised "
		"(rep_cutoff=%d, adapt_window=%d, adapt_cutoff=%d)\n",
		REP_COUNT_CUTOFF, ADAPT_WINDOW_SIZE, ADAPT_CUTOFF);
	return 0;
}

void sl_health_cleanup(void)
{
	kfree(g_health);
	g_health = NULL;
}

/*
 * Run both NIST SP 800-90B health tests on a block of entropy data.
 *
 * @data: pointer to entropy bytes
 * @len:  number of bytes
 *
 * Returns 0 if all tests pass, -EIO if any test fails.
 *
 * On failure, the caller should discard the data and retry collection.
 * A persistent failure indicates a hardware malfunction.
 */
int sl_health_test(const u8 *data, size_t len)
{
	struct health_state *hs = g_health;
	size_t i;
	int ret = 0;
	unsigned long flags;

	if (!hs)
		return 0; /* no health testing — pass through */

	spin_lock_irqsave(&hs->lock, flags);

	for (i = 0; i < len; i++) {
		u8 sample = data[i];

		atomic64_inc(&hs->total_samples);

		if (repetition_count_test(hs, sample) < 0) {
			atomic64_inc(&hs->failed_samples);
			ret = -EIO;
			/*
			 * Don't break — continue testing so the state
			 * machine stays synchronized with the data stream.
			 */
		}

		if (adaptive_proportion_test(hs, sample) < 0) {
			atomic64_inc(&hs->failed_samples);
			ret = -EIO;
		}
	}

	spin_unlock_irqrestore(&hs->lock, flags);
	return ret;
}

void sl_health_get_stats(u64 *total_samples, u64 *failed_samples,
			 u64 *rep_failures, u64 *prop_failures)
{
	if (!g_health) {
		*total_samples  = 0;
		*failed_samples = 0;
		*rep_failures   = 0;
		*prop_failures  = 0;
		return;
	}

	*total_samples  = atomic64_read(&g_health->total_samples);
	*failed_samples = atomic64_read(&g_health->failed_samples);
	*rep_failures   = atomic64_read(&g_health->rep_failures);
	*prop_failures  = atomic64_read(&g_health->prop_failures);
}
