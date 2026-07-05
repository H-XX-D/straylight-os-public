// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — SGX Enclave Kernel Extensions
 * Copyright (C) 2026 StrayLight Systems
 *
 * Detects SGX via CPUID, provides misc device for enclave management,
 * coordinates EPC allocation, sealed storage, and attestation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <asm/cpufeature.h>

#include "enclave.h"

/* ---- ioctl request structures ------------------------------------------ */

#define SGX_IOC_MAGIC           'S'

struct sl_sgx_create_req {
	__u64 size;
	__u64 base_addr;
	__u32 enclave_id;
	__u32 flags;
};

struct sl_sgx_add_page_req {
	__u32 enclave_id;
	__u32 page_type;
	__u64 offset;
	__u64 src_addr;
	__u64 flags;
};

struct sl_sgx_init_req {
	__u32 enclave_id;
	__u32 pad;
	__u64 sigstruct_addr;
	__u64 token_addr;
};

struct sl_sgx_seal_req {
	__u32 enclave_id;
	__u32 pad;
	__u64 plaintext_addr;
	__u64 plaintext_size;
	__u64 sealed_addr;
	__u64 sealed_size;
};

struct sl_sgx_unseal_req {
	__u32 enclave_id;
	__u32 pad;
	__u64 sealed_addr;
	__u64 sealed_size;
	__u64 plaintext_addr;
	__u64 plaintext_size;
};

struct sl_sgx_report_req {
	__u32 enclave_id;
	__u32 pad;
	__u64 target_info_addr;
	__u64 report_data_addr;
	__u64 report_addr;
};

struct sl_sgx_destroy_req {
	__u32 enclave_id;
	__u32 pad;
};

#define SGX_IOC_CREATE   _IOWR(SGX_IOC_MAGIC, 0x01, struct sl_sgx_create_req)
#define SGX_IOC_ADD_PAGE _IOW (SGX_IOC_MAGIC, 0x02, struct sl_sgx_add_page_req)
#define SGX_IOC_INIT     _IOW (SGX_IOC_MAGIC, 0x03, struct sl_sgx_init_req)
#define SGX_IOC_SEAL     _IOWR(SGX_IOC_MAGIC, 0x04, struct sl_sgx_seal_req)
#define SGX_IOC_UNSEAL   _IOWR(SGX_IOC_MAGIC, 0x05, struct sl_sgx_unseal_req)
#define SGX_IOC_REPORT   _IOWR(SGX_IOC_MAGIC, 0x06, struct sl_sgx_report_req)
#define SGX_IOC_DESTROY  _IOW (SGX_IOC_MAGIC, 0x07, struct sl_sgx_destroy_req)

static struct sl_sgx_device *g_sgx;

/* ---- SGX detection ----------------------------------------------------- */

static void detect_sgx(struct sl_sgx_device *dev)
{
#ifdef CONFIG_X86_64
	u32 eax, ebx, ecx, edx;

	cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

	if (!(ebx & (1U << 2))) {
		pr_info("straylight-enclave: SGX not supported by CPU\n");
		return;
	}

	dev->sgx1_supported = true;
	pr_info("straylight-enclave: SGX1 supported\n");

	cpuid_count(0x12, 0, &eax, &ebx, &ecx, &edx);

	if (eax & (1U << 0))
		pr_info("straylight-enclave: SGX1 ECREATE/EADD/EINIT available\n");
	if (eax & (1U << 1)) {
		dev->sgx2_supported = true;
		pr_info("straylight-enclave: SGX2 dynamic page management\n");
	}

	{
		int subleaf;

		for (subleaf = 2; subleaf < 10; subleaf++) {
			cpuid_count(0x12, subleaf, &eax, &ebx, &ecx, &edx);

			if ((eax & 0xF) != 1)
				break;

			dev->epc_base = ((u64)(eax & 0xFFFFF000)) |
					((u64)(ebx & 0x000FFFFF) << 32);
			dev->epc_size = ((u64)(ecx & 0xFFFFF000)) |
					((u64)(edx & 0x000FFFFF) << 32);

			pr_info("straylight-enclave: EPC section: "
				"base=0x%llx size=%llu MiB\n",
				dev->epc_base, dev->epc_size >> 20);
		}
	}
#endif
}

/* ---- Enclave lookup ---------------------------------------------------- */

static struct sl_sgx_enclave *find_enclave(struct sl_sgx_device *dev, u32 id)
{
	struct sl_sgx_enclave *enc;

	list_for_each_entry(enc, &dev->enclave_list, list) {
		if (enc->id == id)
			return enc;
	}
	return NULL;
}

/* ---- ioctl handlers ---------------------------------------------------- */

static long sgx_ioctl_create(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_create_req req;
	struct sl_sgx_enclave *enc;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.size < PAGE_SIZE || !is_power_of_2(req.size))
		return -EINVAL;
	if (req.size > (256ULL << 20))
		return -EINVAL;

	enc = kzalloc(sizeof(*enc), GFP_KERNEL);
	if (!enc)
		return -ENOMEM;

	mutex_init(&enc->lock);
	enc->size = req.size;
	enc->initialized = false;
	enc->nr_pages = 0;
	enc->seal_key_valid = false;

	ret = sl_epc_alloc_page(dev, &enc->secs_page, &enc->secs_phys);
	if (ret) {
		kfree(enc);
		return ret;
	}

	ret = sl_epc_ecreate(enc);
	if (ret) {
		sl_epc_free_page(dev, enc->secs_page);
		kfree(enc);
		return ret;
	}

	mutex_lock(&dev->list_lock);
	enc->id = dev->next_id++;
	enc->base_addr = 0x400000000ULL + (enc->id * (256ULL << 20));
	list_add_tail(&enc->list, &dev->enclave_list);
	mutex_unlock(&dev->list_lock);

	req.enclave_id = enc->id;
	req.base_addr  = enc->base_addr;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	pr_info("straylight-enclave: enclave %u created (size=%llu KiB)\n",
		enc->id, enc->size >> 10);
	return 0;
}

static long sgx_ioctl_add_page(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_add_page_req req;
	struct sl_sgx_enclave *enc;
	void *src_data;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	mutex_unlock(&dev->list_lock);

	if (!enc)
		return -ENOENT;
	if (enc->initialized)
		return -EINVAL;
	if (enc->nr_pages >= SL_SGX_MAX_PAGES)
		return -ENOSPC;

	src_data = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!src_data)
		return -ENOMEM;

	if (copy_from_user(src_data, (void __user *)req.src_addr, PAGE_SIZE)) {
		kfree(src_data);
		return -EFAULT;
	}

	mutex_lock(&enc->lock);
	ret = sl_epc_eadd(enc, req.offset, src_data, req.page_type, req.flags);
	mutex_unlock(&enc->lock);

	kfree(src_data);
	return ret;
}

static long sgx_ioctl_init(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_init_req req;
	struct sl_sgx_enclave *enc;
	void *sigstruct, *token = NULL;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	mutex_unlock(&dev->list_lock);

	if (!enc)
		return -ENOENT;
	if (enc->initialized)
		return -EINVAL;

	sigstruct = kmalloc(1808, GFP_KERNEL);
	if (!sigstruct)
		return -ENOMEM;

	if (copy_from_user(sigstruct, (void __user *)req.sigstruct_addr, 1808)) {
		kfree(sigstruct);
		return -EFAULT;
	}

	if (req.token_addr) {
		token = kmalloc(304, GFP_KERNEL);
		if (!token) {
			kfree(sigstruct);
			return -ENOMEM;
		}
		if (copy_from_user(token, (void __user *)req.token_addr, 304)) {
			kfree(token);
			kfree(sigstruct);
			return -EFAULT;
		}
	}

	mutex_lock(&enc->lock);
	ret = sl_epc_einit(enc, sigstruct, token);
	if (ret == 0)
		enc->initialized = true;
	mutex_unlock(&enc->lock);

	kfree(sigstruct);
	kfree(token);
	return ret;
}

static long sgx_ioctl_seal(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_seal_req req;
	struct sl_sgx_enclave *enc;
	void *plain, *sealed;
	size_t sealed_len;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	mutex_unlock(&dev->list_lock);

	if (!enc || !enc->initialized)
		return -EINVAL;
	if (req.plaintext_size > SL_SEAL_MAX_DATA)
		return -EINVAL;

	plain = kmalloc(req.plaintext_size, GFP_KERNEL);
	if (!plain)
		return -ENOMEM;

	if (copy_from_user(plain, (void __user *)req.plaintext_addr,
			   req.plaintext_size)) {
		kfree(plain);
		return -EFAULT;
	}

	sealed_len = req.sealed_size;
	sealed = kmalloc(sealed_len, GFP_KERNEL);
	if (!sealed) {
		kfree(plain);
		return -ENOMEM;
	}

	mutex_lock(&enc->lock);
	ret = sl_seal_data(enc, plain, req.plaintext_size, sealed, &sealed_len);
	mutex_unlock(&enc->lock);

	if (ret == 0) {
		if (copy_to_user((void __user *)req.sealed_addr,
				 sealed, sealed_len))
			ret = -EFAULT;
		else {
			req.sealed_size = sealed_len;
			if (copy_to_user((void __user *)arg, &req, sizeof(req)))
				ret = -EFAULT;
		}
	}

	memzero_explicit(plain, req.plaintext_size);
	kfree(plain);
	kfree(sealed);
	return ret;
}

static long sgx_ioctl_unseal(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_unseal_req req;
	struct sl_sgx_enclave *enc;
	void *sealed, *plain;
	size_t plain_len;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	mutex_unlock(&dev->list_lock);

	if (!enc || !enc->initialized)
		return -EINVAL;
	if (req.sealed_size > SL_SEAL_MAX_DATA + SL_SEAL_OVERHEAD)
		return -EINVAL;

	sealed = kmalloc(req.sealed_size, GFP_KERNEL);
	if (!sealed)
		return -ENOMEM;

	if (copy_from_user(sealed, (void __user *)req.sealed_addr,
			   req.sealed_size)) {
		kfree(sealed);
		return -EFAULT;
	}

	plain_len = req.plaintext_size;
	plain = kmalloc(plain_len, GFP_KERNEL);
	if (!plain) {
		kfree(sealed);
		return -ENOMEM;
	}

	mutex_lock(&enc->lock);
	ret = sl_unseal_data(enc, sealed, req.sealed_size, plain, &plain_len);
	mutex_unlock(&enc->lock);

	if (ret == 0) {
		if (copy_to_user((void __user *)req.plaintext_addr,
				 plain, plain_len))
			ret = -EFAULT;
		else {
			req.plaintext_size = plain_len;
			if (copy_to_user((void __user *)arg, &req, sizeof(req)))
				ret = -EFAULT;
		}
	}

	memzero_explicit(plain, plain_len);
	kfree(plain);
	kfree(sealed);
	return ret;
}

static long sgx_ioctl_report(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_report_req req;
	struct sl_sgx_enclave *enc;
	u8 target_info[512];
	u8 report_data[64];
	u8 report[432];
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	mutex_unlock(&dev->list_lock);

	if (!enc || !enc->initialized)
		return -EINVAL;

	if (copy_from_user(target_info, (void __user *)req.target_info_addr, 512))
		return -EFAULT;
	if (copy_from_user(report_data, (void __user *)req.report_data_addr, 64))
		return -EFAULT;

	mutex_lock(&enc->lock);
	ret = sl_generate_report(enc, target_info, report_data, report);
	mutex_unlock(&enc->lock);

	if (ret == 0) {
		if (copy_to_user((void __user *)req.report_addr, report, 432))
			ret = -EFAULT;
	}

	memzero_explicit(target_info, sizeof(target_info));
	memzero_explicit(report_data, sizeof(report_data));
	return ret;
}

static long sgx_ioctl_destroy(struct sl_sgx_device *dev, unsigned long arg)
{
	struct sl_sgx_destroy_req req;
	struct sl_sgx_enclave *enc;
	unsigned int i;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->list_lock);
	enc = find_enclave(dev, req.enclave_id);
	if (!enc) {
		mutex_unlock(&dev->list_lock);
		return -ENOENT;
	}
	list_del(&enc->list);
	mutex_unlock(&dev->list_lock);

	mutex_lock(&enc->lock);
	for (i = 0; i < enc->nr_pages; i++) {
		if (enc->epc_pages[i])
			sl_epc_free_page(dev, enc->epc_pages[i]);
	}
	if (enc->secs_page)
		sl_epc_free_page(dev, enc->secs_page);
	mutex_unlock(&enc->lock);

	memzero_explicit(enc->seal_key, sizeof(enc->seal_key));
	kfree(enc);

	pr_info("straylight-enclave: enclave %u destroyed\n", req.enclave_id);
	return 0;
}

/* ---- File operations --------------------------------------------------- */

static int sgx_open(struct inode *inode, struct file *filp)
{
	filp->private_data = g_sgx;
	return 0;
}

static int sgx_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long sgx_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct sl_sgx_device *dev = filp->private_data;

	switch (cmd) {
	case SGX_IOC_CREATE:    return sgx_ioctl_create(dev, arg);
	case SGX_IOC_ADD_PAGE:  return sgx_ioctl_add_page(dev, arg);
	case SGX_IOC_INIT:      return sgx_ioctl_init(dev, arg);
	case SGX_IOC_SEAL:      return sgx_ioctl_seal(dev, arg);
	case SGX_IOC_UNSEAL:    return sgx_ioctl_unseal(dev, arg);
	case SGX_IOC_REPORT:    return sgx_ioctl_report(dev, arg);
	case SGX_IOC_DESTROY:   return sgx_ioctl_destroy(dev, arg);
	default:                return -ENOTTY;
	}
}

static const struct file_operations sgx_fops = {
	.owner          = THIS_MODULE,
	.open           = sgx_open,
	.release        = sgx_release,
	.unlocked_ioctl = sgx_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ---- Module init / exit ------------------------------------------------ */

static int __init sl_sgx_init(void)
{
	int ret;

	g_sgx = kzalloc(sizeof(*g_sgx), GFP_KERNEL);
	if (!g_sgx)
		return -ENOMEM;

	INIT_LIST_HEAD(&g_sgx->enclave_list);
	mutex_init(&g_sgx->list_lock);
	g_sgx->next_id = 1;

	detect_sgx(g_sgx);

	ret = sl_epc_init(g_sgx);
	if (ret)
		pr_warn("straylight-enclave: EPC init returned %d\n", ret);

	g_sgx->miscdev.minor = MISC_DYNAMIC_MINOR;
	g_sgx->miscdev.name  = "straylight-enclave";
	g_sgx->miscdev.fops  = &sgx_fops;

	ret = misc_register(&g_sgx->miscdev);
	if (ret) {
		pr_err("straylight-enclave: misc_register failed (%d)\n", ret);
		kfree(g_sgx);
		g_sgx = NULL;
		return ret;
	}

	pr_info("straylight-enclave: module loaded (SGX1=%d SGX2=%d)\n",
		g_sgx->sgx1_supported, g_sgx->sgx2_supported);
	return 0;
}

static void __exit sl_sgx_exit(void)
{
	struct sl_sgx_enclave *enc, *tmp;

	if (!g_sgx)
		return;

	mutex_lock(&g_sgx->list_lock);
	list_for_each_entry_safe(enc, tmp, &g_sgx->enclave_list, list) {
		unsigned int i;

		pr_warn("straylight-enclave: leaked enclave %u\n", enc->id);
		for (i = 0; i < enc->nr_pages; i++) {
			if (enc->epc_pages[i])
				sl_epc_free_page(g_sgx, enc->epc_pages[i]);
		}
		if (enc->secs_page)
			sl_epc_free_page(g_sgx, enc->secs_page);
		memzero_explicit(enc->seal_key, sizeof(enc->seal_key));
		list_del(&enc->list);
		kfree(enc);
	}
	mutex_unlock(&g_sgx->list_lock);

	sl_epc_cleanup(g_sgx);
	misc_deregister(&g_sgx->miscdev);
	kfree(g_sgx);
	g_sgx = NULL;

	pr_info("straylight-enclave: module unloaded\n");
}

module_init(sl_sgx_init);
module_exit(sl_sgx_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StrayLight Systems");
MODULE_DESCRIPTION("StrayLight SGX Enclave Kernel Extensions");
MODULE_VERSION("1.0.0");
