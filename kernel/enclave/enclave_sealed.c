// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — SGX Sealed Storage
 * Copyright (C) 2026 StrayLight Systems
 *
 * Sealed storage binds encrypted data to an enclave identity so that
 * only an enclave with the same measurement (MRENCLAVE) or the same
 * signing key (MRSIGNER) can decrypt it.
 *
 * The sealing key is derived inside the enclave via EGETKEY (an ENCLU
 * instruction, ring-3 only).  This kernel module handles the ioctl
 * plumbing: validating arguments, moving data between userspace and
 * kernel bounce buffers, and invoking the seal/unseal ecalls inside
 * the enclave via EENTER.
 *
 * Sealed blob layout written to the output buffer:
 *
 *   [struct sl_seal_header (16 B)][IV (16 B)][ciphertext][MAC (16 B)]
 *
 *   SL_SEAL_OVERHEAD = 48 bytes
 *
 * The actual AES-128-GCM operation and EGETKEY call run inside the
 * enclave.  In this stub, a deterministic XOR is used in place of
 * AES-GCM so the ioctl path can be exercised on non-SGX machines.
 *
 * Reference: Intel SGX Developer Guide, Chapter 6 (Sealing).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/string.h>

#include "enclave.h"

/* ---- Sealed blob header ----------------------------------------------- */

#define SL_SEAL_VERSION         0x0001

struct sl_seal_header {
	__le16  version;        /* SL_SEAL_VERSION                    */
	__le16  key_policy;     /* MRSIGNER=0x02, MRENCLAVE=0x01      */
	__u8    key_id[12];     /* random salt mixed into KEYREQUEST   */
	__le32  plaintext_len;  /* original plaintext length in bytes  */
} __packed;

static_assert(sizeof(struct sl_seal_header) == 20,
	      "sl_seal_header must be 20 bytes");

/* ---- sl_seal_data ----------------------------------------------------- */

/*
 * sl_seal_data — encrypt plaintext and write a sealed blob.
 *
 * @enc:        the enclave that will own the sealed blob
 * @plain:      plaintext (already in kernel address space)
 * @plain_len:  length in bytes
 * @sealed:     output buffer (at least plain_len + SL_SEAL_OVERHEAD bytes)
 * @sealed_len: in = capacity of sealed buffer; out = bytes written
 *
 * Returns 0 on success, negative errno on error.
 */
int sl_seal_data(struct sl_sgx_enclave *enc,
		 const void *plain, size_t plain_len,
		 void *sealed, size_t *sealed_len)
{
	struct sl_seal_header hdr;
	u8 iv[16];
	u8 mac[16];
	u8 *out;
	const u8 *in = plain;
	size_t required;
	size_t i;

	if (!enc || !plain || !sealed || !sealed_len)
		return -EINVAL;
	if (plain_len == 0 || plain_len > SL_SEAL_MAX_DATA)
		return -EINVAL;

	required = plain_len + SL_SEAL_OVERHEAD;
	if (*sealed_len < required)
		return -ENOSPC;

	/* Generate a random IV / key_id */
	get_random_bytes(iv, sizeof(iv));

	/* Build header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.version      = cpu_to_le16(SL_SEAL_VERSION);
	hdr.key_policy   = cpu_to_le16(0x0002);  /* MRSIGNER */
	memcpy(hdr.key_id, iv, sizeof(hdr.key_id));
	hdr.plaintext_len = cpu_to_le32((u32)plain_len);

	out = sealed;

	/* Write header (16 bytes) */
	memcpy(out, &hdr, sizeof(hdr));
	out += sizeof(hdr);

	/* Write IV (16 bytes — stores remainder of iv[] after key_id) */
	memcpy(out, iv + sizeof(hdr.key_id), 16 - sizeof(hdr.key_id));
	memset(out + (16 - sizeof(hdr.key_id)), 0,
	       sizeof(hdr.key_id));   /* pad to 16 */
	out += 16;

	/*
	 * Production path: EENTER → enclave seal ecall:
	 *   1. EGETKEY(KEYREQUEST{keyname=SEAL, keypolicy, key_id, cpusvn})
	 *      returns a 128-bit sealing key.
	 *   2. AES-128-GCM-Encrypt(key, iv, plaintext, aad=header) →
	 *      (ciphertext, mac).
	 *   3. EEXIT.
	 *
	 * Stub: XOR each byte with 0xA5 and derive a trivial MAC.
	 */
	for (i = 0; i < plain_len; i++)
		out[i] = in[i] ^ 0xA5;
	out += plain_len;

	/* Placeholder MAC: hash of (enc->id XOR first 16 bytes of IV) */
	memcpy(mac, iv, 16);
	for (i = 0; i < 16; i++)
		mac[i] ^= ((u8 *)&enc->id)[i % sizeof(enc->id)];
	memcpy(out, mac, 16);

	*sealed_len = required;
	memzero_explicit(iv, sizeof(iv));
	memzero_explicit(mac, sizeof(mac));
	return 0;
}

/* ---- sl_unseal_data --------------------------------------------------- */

/*
 * sl_unseal_data — decrypt a sealed blob and return plaintext.
 *
 * @enc:        the enclave that created the sealed blob
 * @sealed:     sealed blob (in kernel address space)
 * @sealed_len: total size of sealed blob in bytes
 * @plain:      output buffer (at least sealed_len - SL_SEAL_OVERHEAD bytes)
 * @plain_len:  in = capacity of plain buffer; out = bytes written
 *
 * Returns 0 on success, negative errno on error.
 * Returns -EBADMSG if the MAC verification fails.
 */
int sl_unseal_data(struct sl_sgx_enclave *enc,
		   const void *sealed, size_t sealed_len,
		   void *plain, size_t *plain_len)
{
	struct sl_seal_header hdr;
	const u8 *in;
	u8 *out = plain;
	u8 stored_mac[16];
	u8 computed_mac[16];
	u8 iv[16];
	u32 pt_len;
	size_t i;

	if (!enc || !sealed || !plain || !plain_len)
		return -EINVAL;
	if (sealed_len < SL_SEAL_OVERHEAD)
		return -EINVAL;

	in = sealed;

	/* Parse header */
	memcpy(&hdr, in, sizeof(hdr));
	in += sizeof(hdr);

	if (le16_to_cpu(hdr.version) != SL_SEAL_VERSION) {
		pr_debug("straylight-enclave: unseal: bad version 0x%04x\n",
			 le16_to_cpu(hdr.version));
		return -EINVAL;
	}

	pt_len = le32_to_cpu(hdr.plaintext_len);
	if (pt_len != sealed_len - SL_SEAL_OVERHEAD) {
		pr_debug("straylight-enclave: unseal: length mismatch\n");
		return -EINVAL;
	}
	if (*plain_len < pt_len)
		return -ENOSPC;

	/* Read IV field */
	memcpy(iv, hdr.key_id, sizeof(hdr.key_id));
	memcpy(iv + sizeof(hdr.key_id), in,
	       16 - sizeof(hdr.key_id));
	in += 16;  /* advance past IV field */

	/* Read stored MAC (last 16 bytes of the blob) */
	memcpy(stored_mac,
	       (const u8 *)sealed + sealed_len - 16,
	       16);

	/*
	 * Production path: EENTER → enclave unseal ecall:
	 *   1. EGETKEY(same KEYREQUEST as seal) → 128-bit sealing key.
	 *   2. AES-128-GCM-Decrypt(key, iv, ciphertext, aad=header) →
	 *      (plaintext, mac_check).
	 *   3. Compare mac_check == stored_mac; reject on mismatch.
	 *   4. EEXIT.
	 *
	 * Stub: reverse XOR and verify trivial MAC.
	 */
	for (i = 0; i < pt_len; i++)
		out[i] = in[i] ^ 0xA5;

	/* Recompute MAC with same formula as seal */
	memcpy(computed_mac, iv, 16);
	for (i = 0; i < 16; i++)
		computed_mac[i] ^= ((u8 *)&enc->id)[i % sizeof(enc->id)];

	/* Constant-time MAC comparison */
	{
		u8 diff = 0;

		for (i = 0; i < 16; i++)
			diff |= (stored_mac[i] ^ computed_mac[i]);
		if (diff) {
			pr_warn_ratelimited(
				"straylight-enclave: unseal MAC mismatch "
				"(enc id=%u)\n", enc->id);
			memzero_explicit(out, pt_len);
			memzero_explicit(iv, sizeof(iv));
			memzero_explicit(computed_mac, sizeof(computed_mac));
			return -EBADMSG;
		}
	}

	*plain_len = pt_len;
	memzero_explicit(iv, sizeof(iv));
	memzero_explicit(stored_mac, sizeof(stored_mac));
	memzero_explicit(computed_mac, sizeof(computed_mac));
	return 0;
}
