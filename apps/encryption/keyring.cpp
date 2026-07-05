// apps/encryption/keyring.cpp
// Keyring CRUD — entries sealed with crypto_secretbox under the master key
#include "keyring.h"

#include <sodium.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <algorithm>

namespace straylight::encryption {

namespace {
using json = nlohmann::json;

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

/// Base64-encode a byte span (sodium's built-in encoder).
std::string b64_encode(const uint8_t* data, size_t len) {
    const size_t encoded_len = sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL);
    std::string out(encoded_len, '\0');
    sodium_bin2base64(out.data(), encoded_len, data, len, sodium_base64_VARIANT_ORIGINAL);
    // sodium appends a NUL — trim it
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

/// Base64-decode back to bytes.
std::vector<uint8_t> b64_decode(const std::string& s) {
    std::vector<uint8_t> out(s.size()); // upper bound
    size_t bin_len = 0;
    if (sodium_base642bin(out.data(), out.size(),
                          s.c_str(), s.size(),
                          nullptr, &bin_len,
                          nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return {};
    }
    out.resize(bin_len);
    return out;
}

/// Encrypt `plain` with crypto_secretbox_easy under `key` (random nonce).
/// Returns {ciphertext, nonce}.
std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
box_seal(const uint8_t* plain, size_t plain_len, const std::array<uint8_t,32>& key)
{
    std::vector<uint8_t> nonce(crypto_secretbox_NONCEBYTES);
    randombytes_buf(nonce.data(), nonce.size());

    std::vector<uint8_t> ct(plain_len + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(ct.data(), plain, plain_len, nonce.data(), key.data());
    return {ct, nonce};
}

/// Decrypt with crypto_secretbox_open_easy. Returns empty on failure.
std::vector<uint8_t>
box_open(const std::vector<uint8_t>& ct,
         const std::vector<uint8_t>& nonce,
         const std::array<uint8_t,32>& key)
{
    if (ct.size() < crypto_secretbox_MACBYTES) return {};
    std::vector<uint8_t> plain(ct.size() - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(plain.data(), ct.data(), ct.size(),
                                   nonce.data(), key.data()) != 0) {
        return {};
    }
    return plain;
}

int64_t tp_to_epoch(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point epoch_to_tp(int64_t sec) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{sec}};
}
} // namespace

fs::path Keyring::keyring_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".config" / "straylight"
                         : fs::path("/tmp");
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / "keyring.json";
}

Result<void, SLError> Keyring::load(std::string_view master_pass) {
    // Derive master key
    auto mk_res = Crypto::derive_key(master_pass);
    if (!mk_res.has_value()) {
        return Result<void, SLError>::error(mk_res.error());
    }
    master_key_ = mk_res.value();

    const fs::path path = keyring_path();
    if (!fs::exists(path)) {
        // Fresh keyring — mark as unlocked with empty entries
        unlocked_ = true;
        return Result<void, SLError>::ok();
    }

    std::ifstream f(path);
    if (!f) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot open keyring: " + path.string()));
    }

    json root;
    try { f >> root; }
    catch (const std::exception& ex) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::ParseError,
                     std::string("Keyring JSON parse error: ") + ex.what()));
    }

    entries_.clear();
    for (auto& je : root.value("entries", json::array())) {
        KeyEntry e;
        e.name        = je.value("name", "");
        e.description = je.value("description", "");
        e.encrypted_key = b64_decode(je.value("encrypted_key", ""));
        e.nonce         = b64_decode(je.value("nonce", ""));
        e.salt          = b64_decode(je.value("salt", ""));
        e.created       = epoch_to_tp(je.value("created", int64_t{0}));
        entries_.push_back(std::move(e));
    }

    // Verify master key by trying to unseal the first entry (if any)
    if (!entries_.empty()) {
        auto plain = box_open(entries_[0].encrypted_key,
                              entries_[0].nonce,
                              master_key_.key);
        if (plain.empty()) {
            return Result<void, SLError>::error(
                make_err(SLErrorCode::PermissionDenied, "Wrong master passphrase"));
        }
    }

    unlocked_ = true;
    return Result<void, SLError>::ok();
}

Result<void, SLError> Keyring::save() const {
    if (!unlocked_) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotInitialized, "Keyring is locked"));
    }

    json root;
    json jarr = json::array();
    for (auto& e : entries_) {
        json je;
        je["name"]          = e.name;
        je["description"]   = e.description;
        je["encrypted_key"] = b64_encode(e.encrypted_key.data(), e.encrypted_key.size());
        je["nonce"]         = b64_encode(e.nonce.data(), e.nonce.size());
        je["salt"]          = b64_encode(e.salt.data(), e.salt.size());
        je["created"]       = tp_to_epoch(e.created);
        jarr.push_back(std::move(je));
    }
    root["entries"] = std::move(jarr);

    const fs::path path = keyring_path();
    std::ofstream f(path);
    if (!f) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot write keyring: " + path.string()));
    }
    f << root.dump(2);
    return Result<void, SLError>::ok();
}

Result<void, SLError> Keyring::add(const std::string& name,
                                    const std::string& description,
                                    std::string_view pass) {
    if (!unlocked_) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotInitialized, "Keyring is locked"));
    }

    // Reject duplicate names
    for (auto& e : entries_) {
        if (e.name == name) {
            return Result<void, SLError>::error(
                make_err(SLErrorCode::AlreadyExists,
                         "Key '" + name + "' already exists"));
        }
    }

    // Derive a key from the supplied passphrase
    auto dk_res = Crypto::derive_key(pass);
    if (!dk_res.has_value()) {
        return Result<void, SLError>::error(dk_res.error());
    }
    const DerivedKey dk = dk_res.value();

    // Seal the key bytes under the master key
    auto [ct, nonce] = box_seal(dk.key.data(), dk.key.size(), master_key_.key);

    KeyEntry e;
    e.name          = name;
    e.description   = description;
    e.encrypted_key = std::move(ct);
    e.nonce         = std::move(nonce);
    e.salt          = std::vector<uint8_t>(dk.salt.begin(), dk.salt.end());
    e.created       = std::chrono::system_clock::now();
    entries_.push_back(std::move(e));

    return Result<void, SLError>::ok();
}

Result<void, SLError> Keyring::remove(const std::string& name) {
    if (!unlocked_) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotInitialized, "Keyring is locked"));
    }

    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const KeyEntry& e){ return e.name == name; });
    if (it == entries_.end()) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotFound, "Key '" + name + "' not found"));
    }
    entries_.erase(it);
    return Result<void, SLError>::ok();
}

Result<DerivedKey, SLError> Keyring::unlock(const std::string& name) const {
    if (!unlocked_) {
        return Result<DerivedKey, SLError>::error(
            make_err(SLErrorCode::NotInitialized, "Keyring is locked"));
    }

    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const KeyEntry& e){ return e.name == name; });
    if (it == entries_.end()) {
        return Result<DerivedKey, SLError>::error(
            make_err(SLErrorCode::NotFound, "Key '" + name + "' not found"));
    }

    auto plain = box_open(it->encrypted_key, it->nonce, master_key_.key);
    if (plain.size() != 32) {
        return Result<DerivedKey, SLError>::error(
            make_err(SLErrorCode::Internal,
                     "Failed to unseal key '" + name + "'"));
    }

    DerivedKey dk{};
    std::memcpy(dk.key.data(), plain.data(), 32);
    std::memcpy(dk.salt.data(), it->salt.data(),
                std::min<size_t>(it->salt.size(), dk.salt.size()));
    return Result<DerivedKey, SLError>::ok(dk);
}

} // namespace straylight::encryption
