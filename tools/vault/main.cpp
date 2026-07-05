// tools/vault/main.cpp
// straylight-vault — Encrypted secret manager for StrayLight OS.
// Usage:
//   straylight-vault init
//   straylight-vault set <key> <value>
//   straylight-vault get <key>
//   straylight-vault list [prefix]
//   straylight-vault delete <key>
//   straylight-vault export <file>
//   straylight-vault import <file>
//   straylight-vault lock
//   straylight-vault change-password

#include "vault_store.h"

#include <straylight/log.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>

using straylight::VaultStore;

// ---------------------------------------------------------------------------
// Password prompt (disables terminal echo)
// ---------------------------------------------------------------------------

static std::string read_password(const std::string& prompt) {
    std::cerr << prompt;
    std::cerr.flush();

    struct termios oldt{};
    struct termios newt{};
    bool is_tty = isatty(STDIN_FILENO);

    if (is_tty) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    std::string password;
    std::getline(std::cin, password);

    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cerr << "\n";
    }

    return password;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-vault — Encrypted secret manager\n\n"
        << "Usage:\n"
        << "  straylight-vault init                    Create a new vault\n"
        << "  straylight-vault set <key> <value>       Store a secret\n"
        << "  straylight-vault get <key>               Retrieve a secret\n"
        << "  straylight-vault list [prefix]            List secret keys\n"
        << "  straylight-vault delete <key>            Delete a secret\n"
        << "  straylight-vault export <file>           Export encrypted backup\n"
        << "  straylight-vault import <file>           Import encrypted backup\n"
        << "  straylight-vault change-password         Change master password\n"
        << "\nEnvironment:\n"
        << "  STRAYLIGHT_VAULT_PATH   Override default vault path\n"
        << "  STRAYLIGHT_VAULT_PASS   Master password (avoid; prefer interactive)\n";
}

// ---------------------------------------------------------------------------
// Vault path resolution
// ---------------------------------------------------------------------------

static std::string vault_path() {
    const char* env = std::getenv("STRAYLIGHT_VAULT_PATH");
    return env ? env : VaultStore::default_path();
}

// ---------------------------------------------------------------------------
// Get the master password from env or prompt
// ---------------------------------------------------------------------------

static std::string get_master_password(const std::string& prompt = "Master password: ") {
    const char* env = std::getenv("STRAYLIGHT_VAULT_PASS");
    if (env) return env;
    return read_password(prompt);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_init() {
    VaultStore store;
    std::string path = vault_path();

    std::string pass = get_master_password("New master password: ");
    if (pass.empty()) {
        std::cerr << "Error: password cannot be empty\n";
        return 1;
    }
    std::string confirm = read_password("Confirm master password: ");
    if (pass != confirm) {
        std::cerr << "Error: passwords do not match\n";
        return 1;
    }

    auto r = store.create(path, pass);
    if (!r.has_value()) {
        std::cerr << "Error: " << r.error() << "\n";
        return 1;
    }

    std::cout << "Vault created at " << path << "\n";
    return 0;
}

static int cmd_set(const std::string& key, const std::string& value) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password();
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    auto sr = store.set(key, value);
    if (!sr.has_value()) { std::cerr << "Error: " << sr.error() << "\n"; return 1; }

    std::cout << "Secret stored: " << key << "\n";
    return 0;
}

static int cmd_get(const std::string& key) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password();
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    auto gr = store.get(key);
    if (!gr.has_value()) { std::cerr << "Error: " << gr.error() << "\n"; return 1; }

    std::cout << gr.value() << "\n";
    return 0;
}

static int cmd_list(const std::string& prefix) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password();
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    auto keys = store.list(prefix);
    if (keys.empty()) {
        std::cout << "(no secrets";
        if (!prefix.empty()) std::cout << " matching '" << prefix << "'";
        std::cout << ")\n";
    } else {
        for (const auto& k : keys) {
            std::cout << k << "\n";
        }
    }
    return 0;
}

static int cmd_delete(const std::string& key) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password();
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    auto dr = store.del(key);
    if (!dr.has_value()) { std::cerr << "Error: " << dr.error() << "\n"; return 1; }

    std::cout << "Deleted: " << key << "\n";
    return 0;
}

static int cmd_export(const std::string& file) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password("Vault password: ");
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    std::string export_pass = read_password("Export password: ");
    if (export_pass.empty()) {
        std::cerr << "Error: export password cannot be empty\n";
        return 1;
    }
    std::string confirm = read_password("Confirm export password: ");
    if (export_pass != confirm) {
        std::cerr << "Error: passwords do not match\n";
        return 1;
    }

    auto er = store.export_backup(export_pass);
    if (!er.has_value()) { std::cerr << "Error: " << er.error() << "\n"; return 1; }

    auto blob = std::move(er).value();

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Error: cannot write to " << file << "\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    out.flush();
    if (!out) {
        std::cerr << "Error: write failed\n";
        return 1;
    }

    std::cout << "Exported " << blob.size() << " bytes to " << file << "\n";
    return 0;
}

static int cmd_import(const std::string& file) {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string pass = get_master_password("Vault password: ");
    auto ur = store.unlock(pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    // Read backup file
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "Error: cannot read " << file << "\n";
        return 1;
    }
    std::vector<uint8_t> blob(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());

    std::string import_pass = read_password("Import file password: ");

    auto ir = store.import_backup(blob, import_pass);
    if (!ir.has_value()) { std::cerr << "Error: " << ir.error() << "\n"; return 1; }

    std::cout << "Imported " << ir.value() << " secrets\n";
    return 0;
}

static int cmd_change_password() {
    VaultStore store;
    std::string path = vault_path();

    auto or_ = store.open(path);
    if (!or_.has_value()) { std::cerr << "Error: " << or_.error() << "\n"; return 1; }

    std::string old_pass = read_password("Current password: ");
    auto ur = store.unlock(old_pass);
    if (!ur.has_value()) { std::cerr << "Error: " << ur.error() << "\n"; return 1; }

    std::string new_pass = read_password("New password: ");
    if (new_pass.empty()) {
        std::cerr << "Error: new password cannot be empty\n";
        return 1;
    }
    std::string confirm = read_password("Confirm new password: ");
    if (new_pass != confirm) {
        std::cerr << "Error: passwords do not match\n";
        return 1;
    }

    auto cr = store.change_password(old_pass, new_pass);
    if (!cr.has_value()) { std::cerr << "Error: " << cr.error() << "\n"; return 1; }

    std::cout << "Password changed successfully\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    if (cmd == "init") {
        return cmd_init();
    } else if (cmd == "set") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-vault set <key> <value>\n";
            return 1;
        }
        return cmd_set(argv[2], argv[3]);
    } else if (cmd == "get") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-vault get <key>\n";
            return 1;
        }
        return cmd_get(argv[2]);
    } else if (cmd == "list") {
        std::string prefix = (argc >= 3) ? argv[2] : "";
        return cmd_list(prefix);
    } else if (cmd == "delete") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-vault delete <key>\n";
            return 1;
        }
        return cmd_delete(argv[2]);
    } else if (cmd == "export") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-vault export <file>\n";
            return 1;
        }
        return cmd_export(argv[2]);
    } else if (cmd == "import") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-vault import <file>\n";
            return 1;
        }
        return cmd_import(argv[2]);
    } else if (cmd == "change-password") {
        return cmd_change_password();
    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }
}
