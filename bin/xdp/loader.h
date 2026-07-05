// bin/xdp/loader.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>

struct bpf_object;
struct bpf_program;

namespace straylight::xdp {

/// Loads and attaches XDP BPF programs via libbpf.
class Loader {
public:
    Loader() = default;
    ~Loader();

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
    Loader(Loader&& other) noexcept;
    Loader& operator=(Loader&& other) noexcept;

    /// Load a BPF object file and find the named program within it.
    Result<void, std::string> load(const std::string& path, const std::string& prog_name);

    /// Attach the loaded program to the given interface with XDP flags.
    Result<void, std::string> attach(const std::string& ifname, uint32_t flags);

    /// Detach the program from the currently-attached interface.
    void detach();

    /// Leave the XDP program attached after this loader closes the object.
    void release_attachment() noexcept;

    /// Return the file descriptor of the loaded BPF program, or -1.
    [[nodiscard]] int prog_fd() const noexcept;

    /// Return the interface index we are attached to, or 0.
    [[nodiscard]] int ifindex() const noexcept;

    /// Return whether a program is currently loaded.
    [[nodiscard]] bool loaded() const noexcept;

private:
    bpf_object*  obj_     = nullptr;
    bpf_program* prog_    = nullptr;
    int          prog_fd_ = -1;
    int          ifindex_ = 0;

    void cleanup() noexcept;
};

} // namespace straylight::xdp
