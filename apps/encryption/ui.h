// apps/encryption/ui.h
// EncryptionApp — ImGui tabs: Keys, Encrypt, Decrypt
#pragma once

#include "keyring.h"

#include <future>
#include <string>

namespace straylight::encryption {

/// Full standalone encryption manager application.
/// Manages its own Wayland+EGL+ImGui lifecycle; no AppBase dependency.
class EncryptionApp {
public:
    int run(int argc, char* argv[]);

private:
    Keyring keyring_;

    // Master passphrase input buffer
    char master_buf_[256] = {};
    // Per-operation passphrase buffer
    char pass_buf_[256]   = {};
    // New key name buffer
    char name_buf_[128]   = {};
    // New key description buffer
    char desc_buf_[256]   = {};
    // File path buffers
    char in_path_[1024]   = {};
    char out_path_[1024]  = {};

    std::string status_;
    float progress_ = 0.f;
    int selected_key_ = -1;

    std::future<Result<void, SLError>> active_op_;

    enum class Tab { Keys, Encrypt, Decrypt } tab_ = Tab::Keys;

    void render_unlock();
    void render_keys();
    void render_encrypt();
    void render_decrypt();

    void apply_style();
};

} // namespace straylight::encryption
