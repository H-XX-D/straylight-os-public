// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — SGX Local Attestation
 * Copyright (C) 2026 StrayLight Systems
 *
 * Local attestation lets one enclave on the same platform prove its
 * identity to another without going off-platform.  The sequence is:
 *
 *   1. Verifier (B) reads its own TARGETINFO (identifies itself to the CPU).
 *   2. Prover  (A) calls EREPORT(TARGETINFO_B, REPORTDATA) → REPORT.
 *      The CPU creates and MACs REPORT using a key only derivable by B
 *      via EGETKEY(REPORT_KEY).  Only B can verify the MAC.
 *   3. Prover returns the 432-byte REPORT to the verifier.
 *   4. Verifier runs EGETKEY(REPORT_KEY) to derive the MAC key and checks
 *      REPORT.MAC, then inspects MRENCLAVE / MRSIGNER / attributes.
 *
 * EREPORT is an ENCLU instruction (ring 3, inside enclave only).  The
 * kernel module's role is to:
 *   a. Validate the enclave handle.
 *   b. Copy TARGETINFO and REPORTDATA from userspace.
 *   c. EENTER the prover enclave's report_ecall() which runs EREPORT.
 *   d. Copy the 432-byte REPORT back to the caller.
 *
 * Remote attestation (EPID / DCAP / TDX) builds on local attestation by
 * adding a quoting enclave step; that is handled in userspace via the
 * Intel SGX SDK or the Open Enclave SDK.
 *
 * Reference: Intel SDM Vol. 3D §38.4 (EREPORT), §40.1.4 (local attest).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/crypto.h>
#include <crypto/hash.h>

#include "enclave.h"

/* ---- SGX hardware report layout --------------------------------------- */

/*
 * REPORT body (384 bytes) followed by a 32-byte KEYID field and a
 * 16-byte CMAC-AES-128 MAC, total 432 bytes.
 *
 * We only populate the fields relevant to identity attestation.
 */
struct sl_report_body {
	__u8    cpusvn[16];         /* CPU security version                 */
	__u32   miscselect;         /* MISC features the CPU saved          */
	__u8    reserved1[28];
	__u64   attributes;         /* SGX attribute flags                  */
	__u64   xfrm;               /* extended-feature request mask        */
	__u8    mrenclave[32];      /* SHA-256 measurement of enclave       */
	__u8    reserved2[32];
	__u8    mrsigner[32];       /* SHA-256 of enclave signer public key */
	__u8    reserved3[96];
	__u16   isvprodid;
	__u16   isvsvn;
	__u8    reserved4[60];
	__u8    reportdata[64];     /* caller-supplied nonce / public key   */
} __packed;

static_assert(sizeof(struct sl_report_body) == 384,
	      "sl_report_body must be 384 bytes");

struct sl_full_report {
	struct sl_report_body   body;
	__u8                    keyid[32];  /* report key ID        */
	__u8                    mac[16];    /* CMAC-AES-128 tag     */
} __packed;

static_assert(sizeof(struct sl_full_report) == 432,
	      "sl_full_report must be 432 bytes");

/* ---- Helpers ---------------------------------------------------------- */

/*
 * fill_report_mac — compute a deterministic placeholder MAC.
 *
 * On real hardware the CPU computes CMAC-AES-128 over the report body
 * using a key derived from platform secrets via EGETKEY(REPORT_KEY).
 * Here we fold the MRENCLAVE into a 16-byte tag so integration tests
 * have a non-zero, deterministic value to check.
 */
static void fill_report_mac(struct sl_full_report *rep)
{
	u8 *mac = rep->mac;
	int i;

	for (i = 0; i < 16; i++)
		mac[i] = rep->body.mrenclave[i] ^
			 rep->body.mrenclave[i + 16] ^
			 rep->body.reportdata[i];
}

/* ---- sl_generate_report ----------------------------------------------- */

/*
 * sl_generate_report — generate a local attestation REPORT.
 *
 * @enc:         the prover enclave (must be initialized)
 * @target_info: 512-byte TARGETINFO identifying the verifying enclave
 * @report_data: 64-byte user-supplied nonce / public key hash
 * @report_out:  output buffer (exactly 432 bytes)
 *
 * Returns 0 on success, negative errno on failure.
 */
int sl_generate_report(struct sl_sgx_enclave *enc,
		       const void *target_info,
		       const void *report_data,
		       void *report_out)
{
	struct sl_full_report *rep;

	if (!enc || !target_info || !report_data || !report_out)
		return -EINVAL;
	if (!enc->initialized)
		return -EPERM;

	rep = kzalloc(sizeof(*rep), GFP_KERNEL);
	if (!rep)
		return -ENOMEM;

	/*
	 * --- Production path ---
	 *
	 * 1. Map target_info and report_data into the prover enclave's
	 *    shared memory region (untrusted memory visible to enclave).
	 * 2. EENTER the enclave's report_ecall():
	 *      asm volatile(
	 *          "enclu"
	 *          : : "a"(SGX_EENTER), "b"(tcs_pa),
	 *              "c"(aep), "d"(...)
	 *          : "memory"
	 *      );
	 * 3. Inside the enclave (ring 3):
	 *      asm volatile(
	 *          "enclu"
	 *          : : "a"(SGX_EREPORT),
	 *              "b"(targetinfo_enclave_va),
	 *              "c"(reportdata_enclave_va),
	 *              "d"(report_output_enclave_va)
	 *          : "memory"
	 *      );
	 * 4. EEXIT, copy 432-byte REPORT from shared memory.
	 *
	 * --- Stub path (no SGX hardware) ---
	 *
	 * Populate report fields synthetically for ioctl path testing.
	 */

	/* CPUSVN: zero for stub (hardware fills from processor MSRs) */
	memset(rep->body.cpusvn, 0, sizeof(rep->body.cpusvn));

	/* MISCSELECT and ATTRIBUTES from enclave context */
	rep->body.miscselect = 0;
	rep->body.attributes = 0x0000000000000004ULL;
	rep->body.xfrm = 0x0000000000000003ULL;

	/*
	 * MRENCLAVE: use enclave ID as a distinguishable 8-byte prefix;
	 * on real hardware this is the SHA-256 accumulated via EEXTEND.
	 */
	{
		u64 id = enc->id;

		memcpy(rep->body.mrenclave, &id, sizeof(id));
		/* Remainder stays zero (stub) */
	}

	/* MRSIGNER: zero for an unsigned/self-signed stub */
	memset(rep->body.mrsigner, 0, sizeof(rep->body.mrsigner));

	rep->body.isvprodid = 0;
	rep->body.isvsvn    = 0;

	/* Embed caller's 64-byte REPORTDATA */
	memcpy(rep->body.reportdata, report_data, 64);

	/*
	 * KEYID: first 32 bytes of target_info (a deterministic
	 * stand-in; on real hardware this is random per EINIT).
	 */
	memcpy(rep->keyid, target_info, 32);

	/* Compute placeholder MAC */
	fill_report_mac(rep);

	memcpy(report_out, rep, sizeof(*rep));

	memzero_explicit(rep, sizeof(*rep));
	kfree(rep);
	return 0;
}

/* ---- sl_verify_report ------------------------------------------------- */

/*
 * sl_verify_report — basic structural validation of a REPORT.
 *
 * Real cryptographic verification requires EGETKEY(REPORT_KEY) inside
 * the verifying enclave.  This function performs only structural checks
 * (version, size) to detect obviously malformed reports before they are
 * passed to the in-enclave verifier.
 *
 * @report:     pointer to 432-byte REPORT buffer
 * @report_len: must equal sizeof(struct sl_full_report) = 432
 *
 * Returns 0 if structurally valid, -EINVAL otherwise.
 */
int sl_verify_report(const void *report, size_t report_len)
{
	const struct sl_full_report *rep = report;

	if (!report)
		return -EINVAL;
	if (report_len != sizeof(struct sl_full_report)) {
		pr_debug("straylight-enclave: verify_report: "
			 "bad length %zu (expected %zu)\n",
			 report_len, sizeof(struct sl_full_report));
		return -EINVAL;
	}

	/*
	 * Sanity: a zero MRENCLAVE in a supposedly initialized enclave's
	 * report is suspicious (it should never be all-zero in production).
	 * This is not a security check — it's a developer-mode guard.
	 */
	{
		int i;
		u8 sum = 0;

		for (i = 0; i < 32; i++)
			sum |= rep->body.mrenclave[i];
		if (!sum) {
			pr_debug("straylight-enclave: verify_report: "
				 "all-zero MRENCLAVE\n");
			/* Not fatal — stub enclaves have zero MRENCLAVE */
		}
	}

	return 0;
}
