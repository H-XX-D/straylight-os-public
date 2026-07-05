// tools/cert/cert_manager.h
// TLS certificate manager for StrayLight OS — generate, inspect, trust store.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// Certificate details.
struct CertInfo {
    std::string subject;
    std::string issuer;
    std::string serial;
    std::string not_before;
    std::string not_after;
    int         days_remaining = 0;
    bool        expired = false;
    bool        self_signed = false;
    std::vector<std::string> sans;      // Subject Alternative Names
    std::string fingerprint_sha256;
    std::string public_key_algorithm;
    int         key_bits = 0;
    std::string signature_algorithm;
};

/// Trust store entry.
struct TrustEntry {
    std::string name;
    std::string fingerprint;
    std::string path;
    std::string issuer;
    std::string expiry;
};

/// Certificate expiry warning.
struct ExpiryWarning {
    std::string path;
    std::string subject;
    int         days_remaining = 0;
    std::string not_after;
};

class CertManager {
public:
    CertManager();
    ~CertManager();

    /// Generate a self-signed certificate.
    Result<std::string, std::string> generate(const std::string& common_name,
                                                int days = 365,
                                                const std::string& key_type = "rsa",
                                                int key_bits = 2048,
                                                const std::vector<std::string>& sans = {},
                                                const std::string& output_dir = ".");

    /// Inspect a certificate file or remote host.
    Result<CertInfo, std::string> inspect(const std::string& cert_path_or_host) const;

    /// Add a CA certificate to the system trust store.
    Result<void, std::string> trust_add(const std::string& cert_path);

    /// Remove a CA from the trust store.
    Result<void, std::string> trust_remove(const std::string& name);

    /// List trusted CAs.
    Result<std::vector<TrustEntry>, std::string> trust_list() const;

    /// Check for expiring certificates.
    Result<std::vector<ExpiryWarning>, std::string> expiry_check(
        int warn_days = 30,
        const std::vector<std::string>& paths = {}) const;

    /// Renew certificates via certbot.
    Result<std::string, std::string> renew(const std::string& domain = "") const;

    /// Convert certificate between formats (PEM, DER, PKCS12).
    Result<std::string, std::string> convert(const std::string& input_path,
                                               const std::string& output_format,
                                               const std::string& output_path = "",
                                               const std::string& password = "") const;

private:
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    std::string cert_dir() const;
    CertInfo parse_openssl_text(const std::string& output) const;
};

} // namespace straylight
