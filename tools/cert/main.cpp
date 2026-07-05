// tools/cert/main.cpp
// CLI front-end for straylight-cert — TLS certificate manager.

#include "cert_manager.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-cert — TLS certificate manager CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-cert generate [--cn=X] [--days=N] [--type=rsa|ec] [--bits=2048] [--san=X,Y]\n"
        << "  straylight-cert inspect <cert-or-host>        Inspect certificate\n"
        << "  straylight-cert trust add <cert>              Add CA to trust store\n"
        << "  straylight-cert trust remove <name>           Remove from trust store\n"
        << "  straylight-cert trust list                    List trusted CAs\n"
        << "  straylight-cert expiry [--warn-days=30]       Check for expiring certs\n"
        << "  straylight-cert renew [domain]                Renew via certbot\n"
        << "  straylight-cert convert <cert> <format>       Convert cert format\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::CertManager mgr;

    // -----------------------------------------------------------------------
    // generate
    // -----------------------------------------------------------------------
    if (command == "generate") {
        std::string cn = get_arg(argc, argv, "--cn=");
        if (cn.empty()) cn = "localhost";
        std::string days_str = get_arg(argc, argv, "--days=");
        int days = days_str.empty() ? 365 : std::atoi(days_str.c_str());
        std::string type = get_arg(argc, argv, "--type=");
        if (type.empty()) type = "rsa";
        std::string bits_str = get_arg(argc, argv, "--bits=");
        int bits = bits_str.empty() ? 2048 : std::atoi(bits_str.c_str());
        std::string output = get_arg(argc, argv, "--output=");
        if (output.empty()) output = ".";

        std::vector<std::string> sans;
        std::string san_str = get_arg(argc, argv, "--san=");
        if (!san_str.empty()) {
            std::istringstream ss(san_str);
            std::string s;
            while (std::getline(ss, s, ',')) {
                sans.push_back(s);
            }
        }

        auto res = mgr.generate(cn, days, type, bits, sans, output);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Generated certificate: " << res.value() << "\n"
                  << "  CN:   " << cn << "\n"
                  << "  Days: " << days << "\n"
                  << "  Type: " << type << " " << bits << "-bit\n";
        if (!sans.empty()) {
            std::cout << "  SANs:";
            for (const auto& s : sans) std::cout << " " << s;
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // inspect <cert-or-host>
    // -----------------------------------------------------------------------
    if (command == "inspect") {
        if (argc < 3) {
            std::cerr << "Error: 'inspect' requires a certificate path or hostname\n";
            return 1;
        }
        auto res = mgr.inspect(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& info = res.value();
        std::cout << "Certificate Details\n"
                  << std::string(50, '-') << "\n"
                  << "  Subject:     " << info.subject << "\n"
                  << "  Issuer:      " << info.issuer << "\n"
                  << "  Serial:      " << info.serial << "\n"
                  << "  Not Before:  " << info.not_before << "\n"
                  << "  Not After:   " << info.not_after << "\n";

        if (info.expired) {
            std::cout << "  Status:      \033[31mEXPIRED\033[0m\n";
        } else {
            std::cout << "  Status:      \033[32mValid\033[0m (" << info.days_remaining << " days remaining)\n";
        }

        std::cout << "  Self-Signed: " << (info.self_signed ? "yes" : "no") << "\n"
                  << "  Algorithm:   " << info.public_key_algorithm << " " << info.key_bits << "-bit\n"
                  << "  Signature:   " << info.signature_algorithm << "\n";

        if (!info.sans.empty()) {
            std::cout << "  SANs:\n";
            for (const auto& san : info.sans) {
                std::cout << "    " << san << "\n";
            }
        }

        if (!info.fingerprint_sha256.empty()) {
            std::cout << "  SHA-256:     " << info.fingerprint_sha256 << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // trust add/remove/list
    // -----------------------------------------------------------------------
    if (command == "trust") {
        if (argc < 3) {
            std::cerr << "Error: 'trust' requires a subcommand (add/remove/list)\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "add") {
            if (argc < 4) {
                std::cerr << "Error: 'trust add' requires a certificate path\n";
                return 1;
            }
            auto res = mgr.trust_add(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Certificate added to trust store.\n";
            return 0;
        }

        if (sub == "remove") {
            if (argc < 4) {
                std::cerr << "Error: 'trust remove' requires a certificate name\n";
                return 1;
            }
            auto res = mgr.trust_remove(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Certificate removed from trust store.\n";
            return 0;
        }

        if (sub == "list") {
            auto res = mgr.trust_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& entries = res.value();
            if (entries.empty()) {
                std::cout << "No certificates in trust store.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(30) << "NAME"
                      << std::setw(40) << "ISSUER"
                      << "EXPIRY\n";
            std::cout << std::string(80, '-') << "\n";
            for (const auto& e : entries) {
                std::cout << std::left
                          << std::setw(30) << e.name
                          << std::setw(40) << (e.issuer.empty() ? "-" : e.issuer)
                          << (e.expiry.empty() ? "-" : e.expiry) << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown trust subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // expiry [--warn-days=30]
    // -----------------------------------------------------------------------
    if (command == "expiry") {
        std::string days_str = get_arg(argc, argv, "--warn-days=");
        int warn_days = days_str.empty() ? 30 : std::atoi(days_str.c_str());

        auto res = mgr.expiry_check(warn_days);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& warnings = res.value();
        if (warnings.empty()) {
            std::cout << "No certificates expiring within " << warn_days << " days.\n";
            return 0;
        }

        std::cout << "Certificates expiring within " << warn_days << " days:\n\n";
        for (const auto& w : warnings) {
            std::string status;
            if (w.days_remaining < 0) status = "\033[31mEXPIRED\033[0m";
            else if (w.days_remaining < 7) status = "\033[31m" + std::to_string(w.days_remaining) + " days\033[0m";
            else status = "\033[33m" + std::to_string(w.days_remaining) + " days\033[0m";

            std::cout << "  " << status << "  " << w.subject << "\n"
                      << "         " << w.path << "\n"
                      << "         Expires: " << w.not_after << "\n\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // renew [domain]
    // -----------------------------------------------------------------------
    if (command == "renew") {
        std::string domain = (argc >= 3) ? argv[2] : "";
        auto res = mgr.renew(domain);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // convert <cert> <format> [--output=X] [--password=X]
    // -----------------------------------------------------------------------
    if (command == "convert") {
        if (argc < 4) {
            std::cerr << "Error: 'convert' requires <cert> <format>\n";
            return 1;
        }
        std::string output = get_arg(argc, argv, "--output=", 4);
        std::string password = get_arg(argc, argv, "--password=", 4);

        auto res = mgr.convert(argv[2], argv[3], output, password);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Converted to: " << res.value() << "\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
