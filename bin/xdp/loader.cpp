// bin/xdp/loader.cpp
#include "loader.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <net/if.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace straylight::xdp {

Loader::~Loader() { cleanup(); }

Loader::Loader(Loader&& other) noexcept
    : obj_(other.obj_),
      prog_(other.prog_),
      prog_fd_(other.prog_fd_),
      ifindex_(other.ifindex_) {
    other.obj_     = nullptr;
    other.prog_    = nullptr;
    other.prog_fd_ = -1;
    other.ifindex_ = 0;
}

Loader& Loader::operator=(Loader&& other) noexcept {
    if (this != &other) {
        cleanup();
        obj_     = other.obj_;
        prog_    = other.prog_;
        prog_fd_ = other.prog_fd_;
        ifindex_ = other.ifindex_;
        other.obj_     = nullptr;
        other.prog_    = nullptr;
        other.prog_fd_ = -1;
        other.ifindex_ = 0;
    }
    return *this;
}

Result<void, std::string> Loader::load(const std::string& path, const std::string& prog_name) {
    if (obj_) {
        return Result<void, std::string>::error("BPF object already loaded; detach first");
    }

    // Open the ELF object file
    struct bpf_object* obj = bpf_object__open_file(path.c_str(), nullptr);
    if (!obj) {
        return Result<void, std::string>::error(
            "Failed to open BPF object '" + path + "': " + std::strerror(errno));
    }

    // Load all programs and maps into the kernel
    int err = bpf_object__load(obj);
    if (err) {
        bpf_object__close(obj);
        return Result<void, std::string>::error(
            "Failed to load BPF object: " + std::string(std::strerror(-err)));
    }

    // Find the specific program by section/name
    struct bpf_program* prog = bpf_object__find_program_by_name(obj, prog_name.c_str());
    if (!prog) {
        bpf_object__close(obj);
        return Result<void, std::string>::error(
            "BPF program '" + prog_name + "' not found in object");
    }

    int fd = bpf_program__fd(prog);
    if (fd < 0) {
        bpf_object__close(obj);
        return Result<void, std::string>::error(
            "Failed to get fd for BPF program '" + prog_name + "'");
    }

    obj_     = obj;
    prog_    = prog;
    prog_fd_ = fd;
    return Result<void, std::string>::ok();
}

Result<void, std::string> Loader::attach(const std::string& ifname, uint32_t flags) {
    if (prog_fd_ < 0) {
        return Result<void, std::string>::error("No BPF program loaded");
    }

    unsigned int ifindex = if_nametoindex(ifname.c_str());
    if (ifindex == 0) {
        return Result<void, std::string>::error(
            "Interface '" + ifname + "' not found: " + std::strerror(errno));
    }

    // libbpf 0.5 uses bpf_set_link_xdp_fd; 0.7+ added bpf_xdp_attach
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
    int err = bpf_xdp_attach(static_cast<int>(ifindex), prog_fd_, flags, nullptr);
#else
    int err = bpf_set_link_xdp_fd(static_cast<int>(ifindex), prog_fd_,
                                    static_cast<uint32_t>(flags));
#endif
    if (err) {
        return Result<void, std::string>::error(
            "Failed to attach XDP to '" + ifname + "': " + std::strerror(-err));
    }

    ifindex_ = static_cast<int>(ifindex);
    return Result<void, std::string>::ok();
}

void Loader::detach() {
    if (ifindex_ > 0 && prog_fd_ >= 0) {
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
        bpf_xdp_detach(ifindex_, 0, nullptr);
#else
        bpf_set_link_xdp_fd(ifindex_, -1, 0);
#endif
        ifindex_ = 0;
    }
    cleanup();
}

void Loader::release_attachment() noexcept {
    ifindex_ = 0;
}

int Loader::prog_fd() const noexcept {
    return prog_fd_;
}

int Loader::ifindex() const noexcept {
    return ifindex_;
}

bool Loader::loaded() const noexcept {
    return obj_ != nullptr;
}

void Loader::cleanup() noexcept {
    if (ifindex_ > 0 && prog_fd_ >= 0) {
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
        bpf_xdp_detach(ifindex_, 0, nullptr);
#else
        bpf_set_link_xdp_fd(ifindex_, -1, 0);
#endif
        ifindex_ = 0;
    }
    if (obj_) {
        bpf_object__close(obj_);
        obj_     = nullptr;
        prog_    = nullptr;
        prog_fd_ = -1;
    }
}

} // namespace straylight::xdp
