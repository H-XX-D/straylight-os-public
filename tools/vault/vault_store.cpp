// tools/vault/vault_store.cpp
// Encrypted secret store implementation.
#include "vault_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string VaultStore::default_path() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/vault.db";
}

std::string VaultStore::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%FT%TZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Create / Open / Unlock / Lock
// ---------------------------------------------------------------------------

Result<void, std::string> VaultStore::create(const std::string& path,
                                              const std::string& master_password) {
    if (master_password.empty()) {
        return Result<void, std::string>::error("Master password cannot be empty");
    }

    if (std::filesystem::exists(path)) {
        return Result<void, std::string>::error("Vault already exists at " + path);
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Result<void, std::string>::error(
                "Cannot create directory " + parent.string() + ": " + ec.message());
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    master_password_ = master_password;
    entries_.clear();
    unlocked_ = true;
    last_access_ = std::chrono::steady_clock::now();

    return save();
}

Result<void, std::string> VaultStore::open(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::error("Vault not found: " + path);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<void, std::string>::error("Cannot open vault file: " + path);
    }

    std::lock_guard<std::mutex> lk(mu_);

    raw_encrypted_.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
    path_ = path;
    unlocked_ = false;
    entries_.clear();

    return Result<void, std::string>::ok();
}

Result<void, std::string> VaultStore::unlock(const std::string& master_password) {
    std::lock_guard<std::mutex> lk(mu_);

    if (raw_encrypted_.empty()) {
        return Result<void, std::string>::error("No vault loaded — call open() first");
    }

    auto dec = VaultCrypto::decrypt(raw_encrypted_, master_password);
    if (!dec.has_value()) {
        return Result<void, std::string>::error("Unlock failed: " + dec.error());
    }

    // Parse the decrypted JSON
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(dec.value());
    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::error(
            std::string("Vault data corrupted: ") + e.what());
    }

    entries_.clear();
    if (doc.contains("secrets") && doc["secrets"].is_object()) {
        for (auto& [k, v] : doc["secrets"].items()) {
            VaultEntry entry;
            entry.key = k;
            entry.value = v.value("value", "");
            entry.created_at = v.value("created_at", "");
            entry.updated_at = v.value("updated_at", "");
            entries_[k] = std::move(entry);
        }
    }

    master_password_ = master_password;
    unlocked_ = true;
    last_access_ = std::chrono::steady_clock::now();

    return Result<void, std::string>::ok();
}

void VaultStore::lock() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    master_password_.clear();
    unlocked_ = false;
}

bool VaultStore::is_unlocked() const {
    return unlocked_;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

Result<void, std::string> VaultStore::set(const std::string& key,
                                           const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<void, std::string>::error("Vault is locked");
    }

    auto now = now_iso8601();
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.value = value;
        it->second.updated_at = now;
    } else {
        VaultEntry entry;
        entry.key = key;
        entry.value = value;
        entry.created_at = now;
        entry.updated_at = now;
        entries_[key] = std::move(entry);
    }

    last_access_ = std::chrono::steady_clock::now();
    return save();
}

Result<std::string, std::string> VaultStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<std::string, std::string>::error("Vault is locked");
    }

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Result<std::string, std::string>::error("Key not found: " + key);
    }

    last_access_ = std::chrono::steady_clock::now();
    return Result<std::string, std::string>::ok(it->second.value);
}

Result<void, std::string> VaultStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<void, std::string>::error("Vault is locked");
    }

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Result<void, std::string>::error("Key not found: " + key);
    }

    entries_.erase(it);
    last_access_ = std::chrono::steady_clock::now();
    return save();
}

std::vector<std::string> VaultStore::list(const std::string& prefix) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> keys;
    if (!unlocked_) return keys;

    for (const auto& [k, _] : entries_) {
        if (prefix.empty() || k.substr(0, prefix.size()) == prefix) {
            keys.push_back(k);
        }
    }

    last_access_ = std::chrono::steady_clock::now();
    return keys;
}

// ---------------------------------------------------------------------------
// Export / Import
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>, std::string> VaultStore::export_backup(
    const std::string& export_password) const
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<std::vector<uint8_t>, std::string>::error("Vault is locked");
    }

    nlohmann::json doc;
    doc["version"] = 1;
    doc["exported_at"] = now_iso8601();
    nlohmann::json secrets = nlohmann::json::object();
    for (const auto& [k, e] : entries_) {
        nlohmann::json entry;
        entry["value"] = e.value;
        entry["created_at"] = e.created_at;
        entry["updated_at"] = e.updated_at;
        secrets[k] = std::move(entry);
    }
    doc["secrets"] = std::move(secrets);

    std::string plaintext = doc.dump();
    return VaultCrypto::encrypt(plaintext, export_password);
}

Result<int, std::string> VaultStore::import_backup(
    const std::vector<uint8_t>& blob,
    const std::string& export_password)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<int, std::string>::error("Vault is locked");
    }

    auto dec = VaultCrypto::decrypt(blob, export_password);
    if (!dec.has_value()) {
        return Result<int, std::string>::error("Decrypt failed: " + dec.error());
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(dec.value());
    } catch (const nlohmann::json::parse_error& e) {
        return Result<int, std::string>::error(
            std::string("Backup data corrupted: ") + e.what());
    }

    int imported = 0;
    if (doc.contains("secrets") && doc["secrets"].is_object()) {
        auto now = now_iso8601();
        for (auto& [k, v] : doc["secrets"].items()) {
            VaultEntry entry;
            entry.key = k;
            entry.value = v.value("value", "");
            entry.created_at = v.value("created_at", now);
            entry.updated_at = now;
            entries_[k] = std::move(entry);
            ++imported;
        }
    }

    auto sr = save();
    if (!sr.has_value()) {
        return Result<int, std::string>::error("Save failed after import: " + sr.error());
    }

    last_access_ = std::chrono::steady_clock::now();
    return Result<int, std::string>::ok(imported);
}

// ---------------------------------------------------------------------------
// Password change
// ---------------------------------------------------------------------------

Result<void, std::string> VaultStore::change_password(
    const std::string& old_password,
    const std::string& new_password)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!unlocked_) {
        return Result<void, std::string>::error("Vault is locked");
    }
    if (old_password != master_password_) {
        return Result<void, std::string>::error("Old password does not match");
    }
    if (new_password.empty()) {
        return Result<void, std::string>::error("New password cannot be empty");
    }

    master_password_ = new_password;
    return save();
}

// ---------------------------------------------------------------------------
// Idle timeout
// ---------------------------------------------------------------------------

void VaultStore::check_idle_timeout(std::chrono::seconds timeout) {
    if (!unlocked_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_access_);
    if (elapsed >= timeout) {
        lock();
    }
}

void VaultStore::touch() {
    last_access_ = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Result<void, std::string> VaultStore::save() {
    if (path_.empty()) {
        return Result<void, std::string>::error("No vault path set");
    }

    // Build JSON document
    nlohmann::json doc;
    doc["version"] = 1;
    nlohmann::json secrets = nlohmann::json::object();
    for (const auto& [k, e] : entries_) {
        nlohmann::json entry;
        entry["value"] = e.value;
        entry["created_at"] = e.created_at;
        entry["updated_at"] = e.updated_at;
        secrets[k] = std::move(entry);
    }
    doc["secrets"] = std::move(secrets);

    // Encrypt
    std::string plaintext = doc.dump();
    auto enc = VaultCrypto::encrypt(plaintext, master_password_);
    if (!enc.has_value()) {
        return Result<void, std::string>::error("Encryption failed: " + enc.error());
    }

    auto encrypted = std::move(enc).value();

    // Write atomically: write to temp, then rename
    std::string tmp_path = path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Result<void, std::string>::error("Cannot write to " + tmp_path);
        }
        out.write(reinterpret_cast<const char*>(encrypted.data()),
                  static_cast<std::streamsize>(encrypted.size()));
        out.flush();
        if (!out) {
            return Result<void, std::string>::error("Write failed to " + tmp_path);
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path_, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return Result<void, std::string>::error("Rename failed: " + ec.message());
    }

    // Keep raw_encrypted_ up to date for re-open without re-read
    raw_encrypted_ = std::move(encrypted);

    return Result<void, std::string>::ok();
}

} // namespace straylight
