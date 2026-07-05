// bin/rdma_bus/verbs.cpp
#include "verbs.h"

#include <infiniband/verbs.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace straylight::rdma {

VerbsContext::~VerbsContext() { cleanup(); }

VerbsContext::VerbsContext(VerbsContext&& other) noexcept
    : ctx_(other.ctx_),
      pd_(other.pd_),
      comp_channel_(other.comp_channel_),
      cq_(other.cq_),
      mr_count_(other.mr_count_),
      last_rkey_(other.last_rkey_) {
    for (size_t i = 0; i < mr_count_; ++i) {
        mrs_[i] = other.mrs_[i];
    }
    other.ctx_          = nullptr;
    other.pd_           = nullptr;
    other.comp_channel_ = nullptr;
    other.cq_           = nullptr;
    other.mr_count_     = 0;
    other.last_rkey_    = 0;
}

VerbsContext& VerbsContext::operator=(VerbsContext&& other) noexcept {
    if (this != &other) {
        cleanup();
        ctx_          = other.ctx_;
        pd_           = other.pd_;
        comp_channel_ = other.comp_channel_;
        cq_           = other.cq_;
        mr_count_     = other.mr_count_;
        last_rkey_    = other.last_rkey_;
        for (size_t i = 0; i < mr_count_; ++i) {
            mrs_[i] = other.mrs_[i];
        }
        other.ctx_          = nullptr;
        other.pd_           = nullptr;
        other.comp_channel_ = nullptr;
        other.cq_           = nullptr;
        other.mr_count_     = 0;
        other.last_rkey_    = 0;
    }
    return *this;
}

Result<void, std::string> VerbsContext::open(const std::string& device_name) {
    if (ctx_) {
        return Result<void, std::string>::error("Device already open");
    }

    // Get the list of RDMA devices
    int num_devices = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        if (dev_list) {
            ibv_free_device_list(dev_list);
        }
        return Result<void, std::string>::error("No RDMA devices found");
    }

    struct ibv_device* target = nullptr;

    if (device_name.empty()) {
        // Use the first available device
        target = dev_list[0];
    } else {
        // Find the named device
        for (int i = 0; i < num_devices; ++i) {
            if (device_name == ibv_get_device_name(dev_list[i])) {
                target = dev_list[i];
                break;
            }
        }
    }

    if (!target) {
        ibv_free_device_list(dev_list);
        return Result<void, std::string>::error(
            "RDMA device '" + device_name + "' not found");
    }

    ctx_ = ibv_open_device(target);
    ibv_free_device_list(dev_list);

    if (!ctx_) {
        return Result<void, std::string>::error(
            std::string("ibv_open_device failed: ") + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> VerbsContext::create_pd() {
    if (!ctx_) {
        return Result<void, std::string>::error("Device not open");
    }
    if (pd_) {
        return Result<void, std::string>::error("PD already created");
    }

    pd_ = ibv_alloc_pd(ctx_);
    if (!pd_) {
        return Result<void, std::string>::error(
            std::string("ibv_alloc_pd failed: ") + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

Result<uint32_t, std::string> VerbsContext::alloc_mr(void* buf, size_t size, int access) {
    if (!pd_) {
        return Result<uint32_t, std::string>::error("PD not created");
    }
    if (mr_count_ >= MAX_MRS) {
        return Result<uint32_t, std::string>::error("Maximum MR count reached");
    }
    if (!buf || size == 0) {
        return Result<uint32_t, std::string>::error("Invalid buffer for MR registration");
    }

    struct ibv_mr* mr = ibv_reg_mr(pd_, buf, size, access);
    if (!mr) {
        return Result<uint32_t, std::string>::error(
            std::string("ibv_reg_mr failed: ") + std::strerror(errno));
    }

    mrs_[mr_count_++] = MrEntry{mr};
    last_rkey_ = mr->rkey;

    return Result<uint32_t, std::string>::ok(mr->lkey);
}

Result<void, std::string> VerbsContext::create_cq(int cq_depth) {
    if (!ctx_) {
        return Result<void, std::string>::error("Device not open");
    }
    if (cq_) {
        return Result<void, std::string>::error("CQ already created");
    }

    // Create completion channel for event-driven CQ polling
    comp_channel_ = ibv_create_comp_channel(ctx_);
    if (!comp_channel_) {
        return Result<void, std::string>::error(
            std::string("ibv_create_comp_channel failed: ") + std::strerror(errno));
    }

    cq_ = ibv_create_cq(ctx_, cq_depth, nullptr, comp_channel_, 0);
    if (!cq_) {
        ibv_destroy_comp_channel(comp_channel_);
        comp_channel_ = nullptr;
        return Result<void, std::string>::error(
            std::string("ibv_create_cq failed: ") + std::strerror(errno));
    }

    // Arm the CQ for notifications
    int ret = ibv_req_notify_cq(cq_, 0);
    if (ret) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
        ibv_destroy_comp_channel(comp_channel_);
        comp_channel_ = nullptr;
        return Result<void, std::string>::error(
            "ibv_req_notify_cq failed: " + std::to_string(ret));
    }

    return Result<void, std::string>::ok();
}

void VerbsContext::close() {
    cleanup();
}

void VerbsContext::cleanup() noexcept {
    // Deregister all MRs
    for (size_t i = 0; i < mr_count_; ++i) {
        if (mrs_[i].mr) {
            ibv_dereg_mr(mrs_[i].mr);
            mrs_[i].mr = nullptr;
        }
    }
    mr_count_ = 0;

    if (cq_) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
    }
    if (comp_channel_) {
        ibv_destroy_comp_channel(comp_channel_);
        comp_channel_ = nullptr;
    }
    if (pd_) {
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
    }
    if (ctx_) {
        ibv_close_device(ctx_);
        ctx_ = nullptr;
    }
}

} // namespace straylight::rdma
