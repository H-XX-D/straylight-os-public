// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Hardware Entropy Source
 * Copyright (C) 2026 StrayLight Systems
 *
 * Combines CPU timing jitter and RDRAND/RDSEED entropy sources,
 * registers as a hw_random device with quality=1024 (fully trusted).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hw_random.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "entropy.h"

/* ---- Entropy mixing ---------------------------------------------------- */

struct sl_entropy_ctx {
	struct hwrng            hwrng;
	struct mutex            lock;
	struct proc_dir_entry   *proc_dir;

	/* Source availability */
	bool                    has_rdrand;
	bool                    has_rdseed;
	bool                    has_jitter;

	/* Statistics */
	atomic64_t              bytes_generated;
	atomic64_t              rdrand_calls;
	atomic64_t              rdseed_calls;
	atomic64_t              jitter_calls;
	atomic64_t              health_failures;
};

static struct sl_entropy_ctx *g_ent;

/*
 * XOR-fold multiple entropy sources for defense in depth.
 * Even if one source is compromised, the output remains unpredictable
 * as long as at least one source provides true randomness.
 */
static int sl_entropy_read(struct hwrng *rng, void *buf, size_t max,
			   bool wait)
{
	struct sl_entropy_ctx *ctx = (struct sl_entropy_ctx *)rng->priv;
	u8 *out = buf;
	u8 rdrand_buf[64];
	u8 jitter_buf[64];
	size_t chunk, remaining, i;
	int ret;

	remaining = max;
	mutex_lock(&ctx->lock);

	while (remaining > 0) {
		chunk = min_t(size_t, remaining, 64);

		/* Start with zeroed output */
		memset(out, 0, chunk);

		/* Layer 1: RDSEED (highest quality, may fail) */
		if (ctx->has_rdseed) {
			ret = sl_rdseed_read(out, chunk);
			if (ret > 0) {
				atomic64_inc(&ctx->rdseed_calls);
			}
		}

		/* Layer 2: RDRAND */
		if (ctx->has_rdrand) {
			memset(rdrand_buf, 0, sizeof(rdrand_buf));
			ret = sl_rdrand_read(rdrand_buf, chunk);
			if (ret > 0) {
				for (i = 0; i < chunk; i++)
					out[i] ^= rdrand_buf[i];
				atomic64_inc(&ctx->rdrand_calls);
			}
		}

		/* Layer 3: CPU jitter entropy */
		if (ctx->has_jitter) {
			memset(jitter_buf, 0, sizeof(jitter_buf));
			ret = sl_jitter_read(jitter_buf, chunk);
			if (ret > 0) {
				for (i = 0; i < chunk; i++)
					out[i] ^= jitter_buf[i];
				atomic64_inc(&ctx->jitter_calls);
			}
		}

		/* Health check the output */
		ret = sl_health_test(out, chunk);
		if (ret < 0) {
			atomic64_inc(&ctx->health_failures);
			pr_warn_ratelimited("straylight-entropy: "
					    "health test failed, retrying\n");
			/* Don't advance — retry this chunk */
			continue;
		}

		/* Zeroize temporary buffers */
		memzero_explicit(rdrand_buf, sizeof(rdrand_buf));
		memzero_explicit(jitter_buf, sizeof(jitter_buf));

		out       += chunk;
		remaining -= chunk;
		atomic64_add(chunk, &ctx->bytes_generated);
	}

	mutex_unlock(&ctx->lock);
	return max;
}

/* ---- /proc interface --------------------------------------------------- */

static int entropy_proc_show(struct seq_file *m, void *v)
{
	u64 total, failed, rep_fail, prop_fail;

	if (!g_ent)
		return -ENODEV;

	sl_health_get_stats(&total, &failed, &rep_fail, &prop_fail);

	seq_puts(m, "StrayLight Hardware Entropy Source\n");
	seq_puts(m, "==================================\n\n");
	seq_puts(m, "Sources:\n");
	seq_printf(m, "  RDRAND:   %s\n",
		   g_ent->has_rdrand ? "available" : "not available");
	seq_printf(m, "  RDSEED:   %s\n",
		   g_ent->has_rdseed ? "available" : "not available");
	seq_printf(m, "  Jitter:   %s\n\n",
		   g_ent->has_jitter ? "available" : "not available");

	seq_puts(m, "Statistics:\n");
	seq_printf(m, "  Bytes generated:   %lld\n",
		   atomic64_read(&g_ent->bytes_generated));
	seq_printf(m, "  RDRAND calls:      %lld\n",
		   atomic64_read(&g_ent->rdrand_calls));
	seq_printf(m, "  RDSEED calls:      %lld\n",
		   atomic64_read(&g_ent->rdseed_calls));
	seq_printf(m, "  Jitter calls:      %lld\n",
		   atomic64_read(&g_ent->jitter_calls));
	seq_printf(m, "  Health failures:   %lld\n\n",
		   atomic64_read(&g_ent->health_failures));

	seq_puts(m, "Health Tests (NIST SP 800-90B):\n");
	seq_printf(m, "  Total samples:          %llu\n", total);
	seq_printf(m, "  Failed samples:         %llu\n", failed);
	seq_printf(m, "  Repetition count fails: %llu\n", rep_fail);
	seq_printf(m, "  Adaptive prop fails:    %llu\n", prop_fail);

	return 0;
}

static int entropy_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, entropy_proc_show, NULL);
}

static const struct proc_ops entropy_proc_ops = {
	.proc_open    = entropy_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- Module init / exit ------------------------------------------------ */

static int __init sl_entropy_init(void)
{
	int ret;

	g_ent = kzalloc(sizeof(*g_ent), GFP_KERNEL);
	if (!g_ent)
		return -ENOMEM;

	mutex_init(&g_ent->lock);
	atomic64_set(&g_ent->bytes_generated, 0);
	atomic64_set(&g_ent->rdrand_calls, 0);
	atomic64_set(&g_ent->rdseed_calls, 0);
	atomic64_set(&g_ent->jitter_calls, 0);
	atomic64_set(&g_ent->health_failures, 0);

	/* Check source availability */
	g_ent->has_rdrand = sl_rdrand_available();
	g_ent->has_rdseed = sl_rdseed_available();

	/* Init jitter entropy */
	ret = sl_jitter_init();
	g_ent->has_jitter = (ret == 0);

	/* Init health tests */
	ret = sl_health_init();
	if (ret) {
		pr_err("straylight-entropy: health init failed (%d)\n", ret);
		goto err_health;
	}

	/* Require at least one entropy source */
	if (!g_ent->has_rdrand && !g_ent->has_rdseed && !g_ent->has_jitter) {
		pr_err("straylight-entropy: no hardware entropy sources available\n");
		ret = -ENODEV;
		goto err_nosrc;
	}

	/* Register with hw_random subsystem */
	g_ent->hwrng.name    = "straylight-entropy";
	g_ent->hwrng.read    = sl_entropy_read;
	g_ent->hwrng.quality = 1024; /* fully trusted — multi-source */
	g_ent->hwrng.priv    = (unsigned long)g_ent;

	ret = hwrng_register(&g_ent->hwrng);
	if (ret) {
		pr_err("straylight-entropy: hwrng_register failed (%d)\n", ret);
		goto err_hwrng;
	}

	/* /proc interface */
	proc_mkdir("straylight", NULL);
	g_ent->proc_dir = proc_create("straylight/entropy", 0444,
				      NULL, &entropy_proc_ops);

	pr_info("straylight-entropy: registered (RDRAND=%d RDSEED=%d Jitter=%d)\n",
		g_ent->has_rdrand, g_ent->has_rdseed, g_ent->has_jitter);
	return 0;

err_hwrng:
err_nosrc:
	sl_health_cleanup();
err_health:
	sl_jitter_cleanup();
	kfree(g_ent);
	g_ent = NULL;
	return ret;
}

static void __exit sl_entropy_exit(void)
{
	if (!g_ent)
		return;

	hwrng_unregister(&g_ent->hwrng);

	if (g_ent->proc_dir)
		remove_proc_entry("straylight/entropy", NULL);

	sl_health_cleanup();
	sl_jitter_cleanup();

	pr_info("straylight-entropy: %lld bytes generated total\n",
		atomic64_read(&g_ent->bytes_generated));

	kfree(g_ent);
	g_ent = NULL;

	pr_info("straylight-entropy: module unloaded\n");
}

module_init(sl_entropy_init);
module_exit(sl_entropy_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StrayLight Systems");
MODULE_DESCRIPTION("StrayLight Hardware Entropy Source (RDRAND + Jitter)");
MODULE_VERSION("1.0.0");
