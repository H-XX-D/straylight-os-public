/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STRAYLIGHT_ENTROPY_H
#define _STRAYLIGHT_ENTROPY_H

#include <linux/types.h>

int  sl_jitter_init(void);
void sl_jitter_cleanup(void);
int  sl_jitter_read(void *buf, size_t count);

int  sl_rdrand_available(void);
int  sl_rdseed_available(void);
int  sl_rdrand_read(void *buf, size_t count);
int  sl_rdseed_read(void *buf, size_t count);

int  sl_health_init(void);
void sl_health_cleanup(void);
int  sl_health_test(const u8 *data, size_t len);
void sl_health_get_stats(u64 *total_samples, u64 *failed_samples,
				 u64 *rep_failures, u64 *prop_failures);

#endif /* _STRAYLIGHT_ENTROPY_H */
