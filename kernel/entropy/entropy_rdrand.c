// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — RDRAND / RDSEED Instruction Wrappers
 * Copyright (C) 2026 StrayLight Systems
 *
 * Provides RDRAND and RDSEED access with retry loops and
 * fallback handling per Intel's recommendations.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>

#include "entropy.h"

/*
 * Intel recommends retrying RDRAND up to 10 times if the carry flag
 * indicates failure (CF=0).  For RDSEED, more retries may be needed
 * as the seed source can be temporarily depleted.
 */
#define RDRAND_RETRY_COUNT      10
#define RDSEED_RETRY_COUNT      32
#define RDSEED_BACKOFF_LOOPS    16

/* ---- Feature detection ------------------------------------------------- */

int sl_rdrand_available(void)
{
#ifdef CONFIG_X86_64
	u32 ecx = cpuid_ecx(1);

	/* CPUID.01H:ECX.RDRAND[bit 30] */
	if (ecx & (1U << 30)) {
		pr_info("straylight-entropy: RDRAND supported\n");
		return 1;
	}
#endif
	return 0;
}

int sl_rdseed_available(void)
{
#ifdef CONFIG_X86_64
	u32 ebx;

	/* CPUID.07H:EBX.RDSEED[bit 18] */
	ebx = cpuid_ebx(7);
	if (ebx & (1U << 18)) {
		pr_info("straylight-entropy: RDSEED supported\n");
		return 1;
	}
#endif
	return 0;
}

/* ---- RDRAND ------------------------------------------------------------ */

static __always_inline bool rdrand64(u64 *value)
{
#ifdef CONFIG_X86_64
	u8 ok;

	asm volatile("rdrand %[val]; setc %[ok]"
		     : [val] "=r" (*value), [ok] "=qm" (ok)
		     :
		     : "cc");
	return ok;
#else
	(void)value;
	return false;
#endif
}

static __always_inline bool rdrand32(u32 *value)
{
#ifdef CONFIG_X86_64
	u8 ok;

	asm volatile("rdrand %[val]; setc %[ok]"
		     : [val] "=r" (*value), [ok] "=qm" (ok)
		     :
		     : "cc");
	return ok;
#else
	(void)value;
	return false;
#endif
}

static bool rdrand64_retry(u64 *value)
{
	int i;

	for (i = 0; i < RDRAND_RETRY_COUNT; i++) {
		if (rdrand64(value))
			return true;
	}
	return false;
}

int sl_rdrand_read(void *buf, size_t count)
{
	u8 *out = buf;
	size_t generated = 0;
	u64 val;

	while (generated < count) {
		if (!rdrand64_retry(&val))
			break;

		if (count - generated >= sizeof(u64)) {
			memcpy(out + generated, &val, sizeof(u64));
			generated += sizeof(u64);
		} else {
			/* Partial copy for the tail */
			size_t remain = count - generated;

			memcpy(out + generated, &val, remain);
			generated += remain;
		}
	}

	/* Zeroize the last value from the register */
	memzero_explicit(&val, sizeof(val));

	return (int)generated;
}

/* ---- RDSEED ------------------------------------------------------------ */

static __always_inline bool rdseed64(u64 *value)
{
#ifdef CONFIG_X86_64
	u8 ok;

	asm volatile("rdseed %[val]; setc %[ok]"
		     : [val] "=r" (*value), [ok] "=qm" (ok)
		     :
		     : "cc");
	return ok;
#else
	(void)value;
	return false;
#endif
}

/*
 * RDSEED can underflow when the entropy source is exhausted.
 * Intel recommends a pause/backoff loop between retries.
 */
static bool rdseed64_retry(u64 *value)
{
	int i, j;

	for (i = 0; i < RDSEED_RETRY_COUNT; i++) {
		if (rdseed64(value))
			return true;

		/*
		 * Backoff: execute PAUSE instructions to allow the
		 * entropy conditioning logic to refill.
		 */
		for (j = 0; j < RDSEED_BACKOFF_LOOPS * (i + 1); j++)
			cpu_relax();
	}
	return false;
}

int sl_rdseed_read(void *buf, size_t count)
{
	u8 *out = buf;
	size_t generated = 0;
	u64 val;

	while (generated < count) {
		if (!rdseed64_retry(&val))
			break;

		if (count - generated >= sizeof(u64)) {
			memcpy(out + generated, &val, sizeof(u64));
			generated += sizeof(u64);
		} else {
			size_t remain = count - generated;

			memcpy(out + generated, &val, remain);
			generated += remain;
		}
	}

	memzero_explicit(&val, sizeof(val));

	return (int)generated;
}
