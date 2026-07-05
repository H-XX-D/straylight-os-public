// tools/cert/cert_manager.cpp
// Full TLS certificate manager implementation for StrayLight OS.

#include "cert_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

CertManager::CertManager() {
    fs::create_directories(cert_dir());
}

CertManager::~CertManager() = default;

std::string CertManager::cert_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/certs";
}

Result<std::string, std::string> CertManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

// ---------------------------------------------------------------------------
// Parse openssl x509 -text output
// ---------------------------------------------------------------------------

CertInfo CertManager::parse_openssl_text(const std::string& output) const {
    CertInfo info;
    std::istringstream stream(output);
    std::string line;
    bool in_sans = false;

    while (std::getline(stream, line)) {
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos) line = line.substr(pos);

        std::smatch m;

        if (line.rfind("Subject:", 0) == 0) {
            info.subject = line.substr(8);
            auto p = info.subject.find_first_not_of(" \t");
            if (p != std::string::npos) info.subject = info.subject.substr(p);
        } else if (line.rfind("Issuer:", 0) == 0) {
            info.issuer = line.substr(7);
            auto p = info.issuer.find_first_not_of(" \t");
            if (p != std::string::npos) info.issuer = info.issuer.substr(p);
        } else if (line.rfind("Serial Number:", 0) == 0) {
            info.serial = line.substr(14);
            auto p = info.serial.find_first_not_of(" \t");
            if (p != std::string::npos) info.serial = info.serial.substr(p);
        } else if (line.rfind("Not Before:", 0) == 0) {
            info.not_before = line.substr(11);
            auto p = info.not_before.find_first_not_of(" \t");
            if (p != std::string::npos) info.not_before = info.not_before.substr(p);
        } else if (line.rfind("Not After :", 0) == 0) {
            info.not_after = line.substr(11);
            auto p = info.not_after.find_first_not_of(" \t");
            if (p != std::string::npos) info.not_after = info.not_after.substr(p);
        } else if (line.find("Public Key Algorithm:") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                info.public_key_algorithm = line.substr(colon + 2);
            }
        } else if (line.find("Public-Key:") != std::string::npos) {
            std::regex bits_re(R"(\((\d+)\s+bit\))");
            if (std::regex_search(line, m, bits_re)) {
                info.key_bits = std::stoi(m[1].str());
            }
        } else if (line.find("Signature Algorithm:") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                info.signature_algorithm = line.substr(colon + 2);
            }
        } else if (line.find("Subject Alternative Name:") != std::string::npos) {
            in_sans = true;
        } else if (in_sans) {
            // Parse DNS:xxx, IP:xxx entries
            std::regex san_re(R"((DNS|IP Address):([^,\s]+))");
            auto begin = std::sregex_iterator(line.begin(), line.end(), san_re);
            for (auto it = begin; it != std::sregex_iterator(); ++it) {
                info.sans.push_back((*it)[1].str() + ":" + (*it)[2].str());
            }
            in_sans = false;
        }
    }

    info.self_signed = (info.subject == info.issuer);

    // Calculate days remaining
    if (!info.not_after.empty()) {
        // Parse "Mon DD HH:MM:SS YYYY GMT"
        auto end_res = run_cmd("date -d '" + info.not_after + "' +%s 2>/dev/null");
        if (end_res.has_value()) {
            try {
                long end_epoch = std::stol(end_res.value());
                auto now = std::chrono::system_clock::now();
                auto now_epoch = std::chrono::system_clock::to_time_t(now);
                info.days_remaining = static_cast<int>((end_epoch - now_epoch) / 86400);
                info.expired = (info.days_remaining < 0);
            } catch (...) {}
        }
    }

    return info;
}

// ---------------------------------------------------------------------------
// Generate
// ---------------------------------------------------------------------------

Result<std::string, std::string> CertManager::generate(const std::string& common_name,
                                                          int days,
                                                          const std::string& key_type,
                                                          int key_bits,
                                                          const std::vector<std::string>& sans,
                                                          const std::string& output_dir) {
    fs::create_directories(output_dir);

    std::string key_path = output_dir + "/" + common_name + ".key";
    std::string cert_path = output_dir + "/" + common_name + ".crt";

    // Build SAN extension config if needed
    std::string san_config_path;
    if (!sans.empty()) {
        san_config_path = "/tmp/straylight_cert_san_" + common_name + ".cnf";
        std::ofstream san_conf(san_config_path);
        san_conf << "[req]\n"
                 << "distinguished_name = req_distinguished_name\n"
                 << "req_extensions = v3_req\n"
                 << "prompt = no\n"
                 << "\n"
                 << "[req_distinguished_name]\n"
                 << "CN = " << common_name << "\n"
                 << "\n"
                 << "[v3_req]\n"
                 << "subjectAltName = @alt_names\n"
                 << "\n"
                 << "[alt_names]\n";

        int dns_idx = 1, ip_idx = 1;
        san_conf << "DNS." << dns_idx++ << " = " << common_name << "\n";
        for (const auto& san : sans) {
            // Check if IP
            std::regex ip_re(R"(^\d+\.\d+\.\d+\.\d+$)");
            if (std::regex_match(san, ip_re)) {
                san_conf << "IP." << ip_idx++ << " = " << san << "\n";
            } else {
                san_conf << "DNS." << dns_idx++ << " = " << san << "\n";
            }
        }
        san_conf.close();
    }

    // Generate key
    std::string key_cmd;
    if (key_type == "ec" || key_type == "ecdsa") {
        key_cmd = "openssl ecparam -genkey -name prime256v1 -out '" + key_path + "' 2>&1";
    } else {
        key_cmd = "openssl genrsa -out '" + key_path + "' " +
                  std::to_string(key_bits) + " 2>&1";
    }

    auto key_res = run_cmd(key_cmd);
    if (!key_res.has_value()) {
        return Result<std::string, std::string>::error("key generation failed: " + key_res.error());
    }

    // Generate self-signed cert
    std::string cert_cmd = "openssl req -new -x509 -key '" + key_path +
                           "' -out '" + cert_path + "' -days " + std::to_string(days) +
                           " -subj '/CN=" + common_name + "'";

    if (!san_config_path.empty()) {
        cert_cmd += " -config '" + san_config_path + "' -extensions v3_req";
    }
    cert_cmd += " 2>&1";

    auto cert_res = run_cmd(cert_cmd);
    if (!cert_res.has_value()) {
        return Result<std::string, std::string>::error("cert generation failed: " + cert_res.error());
    }

    // Clean up temp config
    if (!san_config_path.empty()) {
        fs::remove(san_config_path);
    }

    return Result<std::string, std::string>::ok(cert_path);
}

// ---------------------------------------------------------------------------
// Inspect
// ---------------------------------------------------------------------------

Result<CertInfo, std::string> CertManager::inspect(const std::string& cert_path_or_host) const {
    std::string openssl_output;

    // Check if it's a file or a host
    if (fs::exists(cert_path_or_host)) {
        auto res = run_cmd("openssl x509 -in '" + cert_path_or_host +
                           "' -text -noout 2>&1");
        if (!res.has_value()) {
            return Result<CertInfo, std::string>::error("failed to read cert: " + res.error());
        }
        openssl_output = res.value();

        // Get fingerprint
        auto fp_res = run_cmd("openssl x509 -in '" + cert_path_or_host +
                              "' -fingerprint -sha256 -noout 2>/dev/null");
        if (fp_res.has_value()) openssl_output += "\n" + fp_res.value();
    } else {
        // Treat as hostname[:port]
        std::string host = cert_path_or_host;
        std::string port = "443";
        auto colon = host.find(':');
        if (colon != std::string::npos) {
            port = host.substr(colon + 1);
            host = host.substr(0, colon);
        }

        auto res = run_cmd("echo | openssl s_client -connect " + host + ":" + port +
                           " -servername " + host +
                           " 2>/dev/null | openssl x509 -text -noout 2>&1");
        if (!res.has_value()) {
            return Result<CertInfo, std::string>::error("failed to connect to " + host + ": " + res.error());
        }
        openssl_output = res.value();
    }

    CertInfo info = parse_openssl_text(openssl_output);

    // Extract fingerprint
    std::regex fp_re(R"(SHA256 Fingerprint=(.+))");
    std::smatch m;
    if (std::regex_search(openssl_output, m, fp_re)) {
        info.fingerprint_sha256 = m[1].str();
    }

    return Result<CertInfo, std::string>::ok(info);
}

// ---------------------------------------------------------------------------
// Trust store management
// ---------------------------------------------------------------------------

Result<void, std::string> CertManager::trust_add(const std::string& cert_path) {
    if (!fs::exists(cert_path)) {
        return Result<void, std::string>::error("certificate not found: " + cert_path);
    }

    // Debian/Ubuntu: /usr/local/share/ca-certificates/
    std::string dest_dir = "/usr/local/share/ca-certificates/straylight/";
    fs::create_directories(dest_dir);

    std::string fname = fs::path(cert_path).filename().string();
    // Ensure .crt extension
    if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".crt") {
        fname += ".crt";
    }

    auto res = run_cmd("cp '" + cert_path + "' '" + dest_dir + fname + "' 2>&1");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to copy cert: " + res.error());
    }

    auto update_res = run_cmd("update-ca-certificates 2>&1");
    if (!update_res.has_value()) {
        return Result<void, std::string>::error("update-ca-certificates failed: " + update_res.error());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CertManager::trust_remove(const std::string& name) {
    std::string dest_dir = "/usr/local/share/ca-certificates/straylight/";
    std::string path = dest_dir + name;
    if (!path.ends_with(".crt")) path += ".crt";

    if (fs::exists(path)) {
        fs::remove(path);
        run_cmd("update-ca-certificates --fresh 2>&1");
        return Result<void, std::string>::ok();
    }

    return Result<void, std::string>::error("certificate '" + name + "' not found in trust store");
}

Result<std::vector<TrustEntry>, std::string> CertManager::trust_list() const {
    std::vector<TrustEntry> entries;

    // List StrayLight-managed certs
    std::string straylight_dir = "/usr/local/share/ca-certificates/straylight/";
    if (fs::exists(straylight_dir)) {
        for (const auto& entry : fs::directory_iterator(straylight_dir)) {
            TrustEntry te;
            te.name = entry.path().filename().string();
            te.path = entry.path().string();

            auto info_res = inspect(te.path);
            if (info_res.has_value()) {
                te.issuer = info_res.value().issuer;
                te.expiry = info_res.value().not_after;
                te.fingerprint = info_res.value().fingerprint_sha256;
            }
            entries.push_back(te);
        }
    }

    // Also list system CAs (summarized)
    auto res = run_cmd("ls /etc/ssl/certs/*.pem 2>/dev/null | head -20");
    if (res.has_value()) {
        std::istringstream stream(res.value());
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            TrustEntry te;
            te.path = line;
            te.name = fs::path(line).filename().string();
            entries.push_back(te);
        }
    }

    return Result<std::vector<TrustEntry>, std::string>::ok(entries);
}

// ---------------------------------------------------------------------------
// Expiry check
// ---------------------------------------------------------------------------

Result<std::vector<ExpiryWarning>, std::string> CertManager::expiry_check(
    int warn_days,
    const std::vector<std::string>& paths) const {

    std::vector<std::string> search_paths = paths;
    if (search_paths.empty()) {
        search_paths = {
            "/etc/ssl/certs",
            "/etc/letsencrypt/live",
            "/usr/local/share/ca-certificates/straylight"
        };
    }

    std::vector<ExpiryWarning> warnings;

    for (const auto& dir : search_paths) {
        if (!fs::exists(dir)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string path = entry.path().string();
            std::string ext = entry.path().extension().string();
            if (ext != ".pem" && ext != ".crt" && ext != ".cert") continue;

            auto info_res = inspect(path);
            if (!info_res.has_value()) continue;

            const auto& info = info_res.value();
            if (info.days_remaining <= warn_days) {
                ExpiryWarning warning;
                warning.path = path;
                warning.subject = info.subject;
                warning.days_remaining = info.days_remaining;
                warning.not_after = info.not_after;
                warnings.push_back(warning);
            }
        }
    }

    // Sort by days remaining (most urgent first)
    std::sort(warnings.begin(), warnings.end(),
              [](const auto& a, const auto& b) {
                  return a.days_remaining < b.days_remaining;
              });

    return Result<std::vector<ExpiryWarning>, std::string>::ok(warnings);
}

// ---------------------------------------------------------------------------
// Renew
// ---------------------------------------------------------------------------

Result<std::string, std::string> CertManager::renew(const std::string& domain) const {
    std::string cmd = "certbot renew";
    if (!domain.empty()) {
        cmd = "certbot certonly --nginx -d " + domain + " --non-interactive";
    }
    cmd += " 2>&1";

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error("certbot failed: " + res.error());
    }
    return Result<std::string, std::string>::ok(res.value());
}

// ---------------------------------------------------------------------------
// Convert
// ---------------------------------------------------------------------------

Result<std::string, std::string> CertManager::convert(const std::string& input_path,
                                                         const std::string& output_format,
                                                         const std::string& output_path,
                                                         const std::string& password) const {
    if (!fs::exists(input_path)) {
        return Result<std::string, std::string>::error("input file not found: " + input_path);
    }

    std::string stem = fs::path(input_path).stem().string();
    std::string out_path = output_path;

    std::string cmd;

    if (output_format == "der") {
        if (out_path.empty()) out_path = stem + ".der";
        cmd = "openssl x509 -in '" + input_path + "' -outform DER -out '" + out_path + "' 2>&1";
    } else if (output_format == "pem") {
        if (out_path.empty()) out_path = stem + ".pem";
        cmd = "openssl x509 -in '" + input_path + "' -inform DER -outform PEM -out '" +
              out_path + "' 2>&1";
    } else if (output_format == "pkcs12" || output_format == "p12") {
        if (out_path.empty()) out_path = stem + ".p12";
        // Need both cert and key
        std::string key_path = fs::path(input_path).parent_path().string() + "/" + stem + ".key";
        cmd = "openssl pkcs12 -export -out '" + out_path +
              "' -inkey '" + key_path + "' -in '" + input_path + "'";
        if (!password.empty()) {
            cmd += " -passout pass:'" + password + "'";
        } else {
            cmd += " -passout pass:''";
        }
        cmd += " 2>&1";
    } else {
        return Result<std::string, std::string>::error("unsupported format: " + output_format +
            " (supported: pem, der, pkcs12)");
    }

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error("conversion failed: " + res.error());
    }

    return Result<std::string, std::string>::ok(out_path);
}

} // namespace straylight
