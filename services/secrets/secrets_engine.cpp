// services/secrets/secrets_engine.cpp
#include "secrets_engine.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace straylight {

namespace fs = std::filesystem;

namespace {

std::string time_to_iso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::chrono::system_clock::time_point parse_iso(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

std::string bytes_to_hex(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        unsigned char b = data[i];
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd-length hex");
    }

    auto nibble = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
        throw std::runtime_error("invalid hex");
    };

    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

std::vector<std::string> split_string(const std::string& value, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(delim, start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

} // namespace

std::array<unsigned char, 32> SecretsEngine::derived_key() const {
    std::array<unsigned char, 32> key{};
    SHA256(reinterpret_cast<const unsigned char*>(master_key_.data()),
           master_key_.size(), key.data());
    return key;
}

std::string SecretsEngine::encrypt(const std::string& plaintext) const {
    auto key = derived_key();

    std::array<unsigned char, 12> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + 16);
    int out_len = 0;
    int final_len = 0;
    std::array<unsigned char, 16> tag{};

    EVP_CIPHER_CTX* raw_ctx = EVP_CIPHER_CTX_new();
    if (!raw_ctx) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
        ctx(raw_ctx, EVP_CIPHER_CTX_free);

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &out_len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + out_len, &final_len) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) != 1) {
        throw std::runtime_error("AES-GCM encryption failed");
    }

    ciphertext.resize(static_cast<size_t>(out_len + final_len));
    return "v2:" + bytes_to_hex(nonce.data(), nonce.size()) + ":" +
           bytes_to_hex(tag.data(), tag.size()) + ":" +
           bytes_to_hex(ciphertext.data(), ciphertext.size());
}

std::string SecretsEngine::decrypt(const std::string& ciphertext) const {
    if (ciphertext.rfind("v2:", 0) == 0) {
        auto parts = split_string(ciphertext, ':');
        if (parts.size() != 4) {
            throw std::runtime_error("invalid encrypted secret format");
        }

        auto nonce = hex_to_bytes(parts[1]);
        auto tag = hex_to_bytes(parts[2]);
        auto encrypted = hex_to_bytes(parts[3]);
        if (nonce.size() != 12 || tag.size() != 16) {
            throw std::runtime_error("invalid AES-GCM nonce/tag size");
        }

        auto key = derived_key();
        std::vector<unsigned char> plaintext(encrypted.size() + 16);
        int out_len = 0;
        int final_len = 0;

        EVP_CIPHER_CTX* raw_ctx = EVP_CIPHER_CTX_new();
        if (!raw_ctx) {
            throw std::runtime_error("EVP_CIPHER_CTX_new failed");
        }
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
            ctx(raw_ctx, EVP_CIPHER_CTX_free);

        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), nullptr) != 1 ||
            EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
            EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len,
                              encrypted.data(), static_cast<int>(encrypted.size())) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                                static_cast<int>(tag.size()), tag.data()) != 1 ||
            EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + out_len, &final_len) != 1) {
            throw std::runtime_error("AES-GCM authentication failed");
        }

        plaintext.resize(static_cast<size_t>(out_len + final_len));
        return std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
    }

    // Backward compatibility for pre-v2 stores that used hex-encoded XOR.
    std::string result;
    result.reserve(ciphertext.size() / 2);
    for (size_t i = 0; i + 1 < ciphertext.size(); i += 2) {
        unsigned char c = static_cast<unsigned char>(
            std::stoi(ciphertext.substr(i, 2), nullptr, 16));
        c ^= static_cast<unsigned char>(master_key_[(i / 2) % master_key_.size()]);
        result += static_cast<char>(c);
    }
    return result;
}

std::string SecretsEngine::generate_random_value(int length) const {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(sizeof(charset) - 2));

    std::string result;
    result.reserve(length);
    for (int i = 0; i < length; ++i) {
        result += charset[dist(gen)];
    }
    return result;
}

bool SecretsEngine::check_access(const SecretEntry& entry, uint32_t uid) const {
    // Root always has access
    if (uid == 0) return true;

    // Check UID list
    if (entry.acl.allowed_uids.count(uid)) return true;

    // Empty ACL means owner-only (uid 0)
    return false;
}

void SecretsEngine::audit(const std::string& action, const std::string& key,
                           uint32_t uid, bool allowed, const std::string& detail) {
    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.action = action;
    entry.key = key;
    entry.uid = uid;
    entry.allowed = allowed;
    entry.detail = detail;
    audit_log_.push_back(std::move(entry));

    // Keep last 10000 entries
    if (audit_log_.size() > 10000) {
        audit_log_.erase(audit_log_.begin(), audit_log_.begin() + 1000);
    }

    if (!allowed) {
        SL_WARN("secrets: ACCESS DENIED uid={} action={} key={}", uid, action, key);
    }
}

Result<void, SLError> SecretsEngine::init(const fs::path& store_path,
                                            const std::string& master_key_path) {
    store_path_ = store_path;

    // Load master key
    std::ifstream kf(master_key_path);
    if (!kf) {
        // Generate a new master key if none exists
        std::array<unsigned char, 32> key_bytes{};
        if (RAND_bytes(key_bytes.data(), static_cast<int>(key_bytes.size())) != 1) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "RAND_bytes failed while generating master key"});
        }
        master_key_ = bytes_to_hex(key_bytes.data(), key_bytes.size());
        std::error_code ec;
        fs::create_directories(fs::path(master_key_path).parent_path(), ec);
        if (ec) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "Failed to create master key directory: " + ec.message()});
        }
        std::ofstream of(master_key_path);
        if (!of) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "Failed to write master key: " + master_key_path});
        }
        of << master_key_ << "\n";
        of.close();
        fs::permissions(master_key_path,
                       fs::perms::owner_read | fs::perms::owner_write, ec);
        if (ec) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "Failed to secure master key permissions: " + ec.message()});
        }
        SL_INFO("secrets: generated new master key at {}", master_key_path);
    } else {
        std::getline(kf, master_key_);
    }

    if (master_key_.empty()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Master key is empty"});
    }
    if (master_key_.size() < 32) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Master key is too short"});
    }

    // Create store directory
    std::error_code ec;
    fs::create_directories(store_path_.parent_path(), ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Failed to create secrets store directory: " + ec.message()});
    }

    load();

    SL_INFO("secrets: initialized with {} secrets", secrets_.size());
    return Result<void, SLError>::ok();
}

Result<void, SLError> SecretsEngine::set(const std::string& key, const std::string& value,
                                          uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it != secrets_.end()) {
        // Update — check access
        if (!check_access(it->second, caller_uid)) {
            audit("set", key, caller_uid, false, "permission denied on update");
            return Result<void, SLError>::error(
                SLError{SLErrorCode::PermissionDenied, "Access denied to key: " + key});
        }
        it->second.encrypted_value = encrypt(value);
        it->second.version++;
        audit("set", key, caller_uid, true, "updated v" + std::to_string(it->second.version));
    } else {
        // Create new
        SecretEntry entry;
        entry.key = key;
        entry.encrypted_value = encrypt(value);
        entry.created = std::chrono::system_clock::now();
        entry.last_rotated = entry.created;
        entry.acl.allowed_uids.insert(caller_uid);
        secrets_[key] = std::move(entry);
        audit("set", key, caller_uid, true, "created");
    }

    return save();
}

Result<std::string, SLError> SecretsEngine::get(const std::string& key,
                                                  uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        audit("get", key, caller_uid, false, "not found");
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::NotFound, "Secret not found: " + key});
    }

    if (!check_access(it->second, caller_uid)) {
        audit("get", key, caller_uid, false, "permission denied");
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Access denied to key: " + key});
    }

    it->second.last_accessed = std::chrono::system_clock::now();
    std::string value = decrypt(it->second.encrypted_value);
    audit("get", key, caller_uid, true);
    return Result<std::string, SLError>::ok(value);
}

Result<void, SLError> SecretsEngine::remove(const std::string& key, uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Secret not found: " + key});
    }

    if (!check_access(it->second, caller_uid)) {
        audit("delete", key, caller_uid, false, "permission denied");
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Access denied to key: " + key});
    }

    secrets_.erase(it);
    audit("delete", key, caller_uid, true);
    return save();
}

std::vector<std::string> SecretsEngine::list(uint32_t caller_uid) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> keys;
    for (const auto& [key, entry] : secrets_) {
        if (check_access(entry, caller_uid)) {
            keys.push_back(key);
        }
    }
    return keys;
}

Result<void, SLError> SecretsEngine::rotate(const std::string& key, uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Secret not found: " + key});
    }

    if (!check_access(it->second, caller_uid)) {
        audit("rotate", key, caller_uid, false, "permission denied");
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Access denied to key: " + key});
    }

    std::string new_value = generate_random_value(32);
    it->second.encrypted_value = encrypt(new_value);
    it->second.last_rotated = std::chrono::system_clock::now();
    it->second.version++;
    audit("rotate", key, caller_uid, true,
          "rotated to v" + std::to_string(it->second.version));
    return save();
}

Result<void, SLError> SecretsEngine::acl_add(const std::string& key, uint32_t target_uid,
                                               uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Secret not found: " + key});
    }

    if (!check_access(it->second, caller_uid)) {
        audit("acl_change", key, caller_uid, false, "permission denied");
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Access denied"});
    }

    it->second.acl.allowed_uids.insert(target_uid);
    audit("acl_change", key, caller_uid, true,
          "added uid " + std::to_string(target_uid));
    return save();
}

Result<void, SLError> SecretsEngine::acl_remove(const std::string& key,
                                                  uint32_t target_uid,
                                                  uint32_t caller_uid) {
    std::lock_guard lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Secret not found: " + key});
    }

    if (!check_access(it->second, caller_uid)) {
        audit("acl_change", key, caller_uid, false, "permission denied");
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Access denied"});
    }

    it->second.acl.allowed_uids.erase(target_uid);
    audit("acl_change", key, caller_uid, true,
          "removed uid " + std::to_string(target_uid));
    return save();
}

std::map<std::string, std::string> SecretsEngine::get_env_for(uint32_t uid) const {
    std::lock_guard lock(mutex_);
    std::map<std::string, std::string> env;
    for (const auto& [key, entry] : secrets_) {
        if (check_access(entry, uid)) {
            // Convert key to env var name: dots/dashes become underscores, uppercase
            std::string env_name = "SL_SECRET_";
            for (char c : key) {
                if (c == '.' || c == '-' || c == '/') env_name += '_';
                else env_name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            env[env_name] = decrypt(entry.encrypted_value);
        }
    }
    return env;
}

Result<void, SLError> SecretsEngine::check_rotations() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::system_clock::now();
    bool changed = false;

    for (auto& [key, entry] : secrets_) {
        if (entry.rotation_interval_hours <= 0) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::hours>(
            now - entry.last_rotated);
        if (elapsed.count() >= entry.rotation_interval_hours) {
            std::string new_value = generate_random_value(32);
            entry.encrypted_value = encrypt(new_value);
            entry.last_rotated = now;
            entry.version++;
            audit("rotate", key, 0, true, "auto-rotated to v" +
                  std::to_string(entry.version));
            SL_INFO("secrets: auto-rotated key '{}'", key);
            changed = true;
        }
    }

    if (!changed) {
        return Result<void, SLError>::ok();
    }
    return save();
}

std::vector<AuditEntry> SecretsEngine::get_audit_log(int last_n) const {
    std::lock_guard lock(mutex_);
    if (static_cast<int>(audit_log_.size()) <= last_n) {
        return audit_log_;
    }
    return std::vector<AuditEntry>(
        audit_log_.end() - last_n, audit_log_.end());
}

Result<void, SLError> SecretsEngine::save() const {
    nlohmann::json j;
    j["secrets"] = nlohmann::json::object();

    for (const auto& [key, entry] : secrets_) {
        nlohmann::json ej;
        ej["encrypted_value"] = entry.encrypted_value;
        ej["version"] = entry.version;
        ej["rotation_interval_hours"] = entry.rotation_interval_hours;
        ej["created"] = time_to_iso(entry.created);
        ej["last_rotated"] = time_to_iso(entry.last_rotated);

        nlohmann::json uids = nlohmann::json::array();
        for (uint32_t uid : entry.acl.allowed_uids) {
            uids.push_back(uid);
        }
        ej["allowed_uids"] = uids;

        nlohmann::json gids = nlohmann::json::array();
        for (uint32_t gid : entry.acl.allowed_gids) {
            gids.push_back(gid);
        }
        ej["allowed_gids"] = gids;

        j["secrets"][key] = ej;
    }

    std::error_code ec;
    fs::create_directories(store_path_.parent_path(), ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Failed to create secrets store directory: " + ec.message()});
    }

    std::ofstream ofs(store_path_, std::ios::trunc);
    if (!ofs) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Failed to write secrets store: " + store_path_.string()});
    }

    ofs << j.dump(2) << "\n";
    ofs.close();
    fs::permissions(store_path_,
                   fs::perms::owner_read | fs::perms::owner_write, ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Failed to secure secrets store permissions: " + ec.message()});
    }

    return Result<void, SLError>::ok();
}

void SecretsEngine::load() {
    if (!fs::exists(store_path_)) return;

    std::ifstream ifs(store_path_);
    if (!ifs) return;

    try {
        nlohmann::json j;
        ifs >> j;

        if (j.contains("secrets") && j["secrets"].is_object()) {
            for (auto& [key, ej] : j["secrets"].items()) {
                SecretEntry entry;
                entry.key = key;
                entry.encrypted_value = ej.value("encrypted_value", "");
                entry.version = ej.value("version", 1);
                entry.rotation_interval_hours = ej.value("rotation_interval_hours", 0);
                entry.created = parse_iso(ej.value("created", "2000-01-01T00:00:00"));
                entry.last_rotated = parse_iso(ej.value("last_rotated", "2000-01-01T00:00:00"));

                if (ej.contains("allowed_uids") && ej["allowed_uids"].is_array()) {
                    for (const auto& uid : ej["allowed_uids"]) {
                        entry.acl.allowed_uids.insert(uid.get<uint32_t>());
                    }
                }
                if (ej.contains("allowed_gids") && ej["allowed_gids"].is_array()) {
                    for (const auto& gid : ej["allowed_gids"]) {
                        entry.acl.allowed_gids.insert(gid.get<uint32_t>());
                    }
                }

                secrets_[key] = std::move(entry);
            }
        }
    } catch (const std::exception& e) {
        SL_WARN("secrets: failed to load store: {}", e.what());
    }
}

} // namespace straylight
