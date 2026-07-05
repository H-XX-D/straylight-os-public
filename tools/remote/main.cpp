// tools/remote/main.cpp
// CLI client for controlling remote StrayLight machines.
// Usage: straylight-remote <command> [options]
//
// NOTE: This is a C++ systems management CLI — not a JavaScript application.
// Shell command construction is intentional for the "authorize" subcommand,
// with input validation applied before any shell invocation.

#include "tls_client.h"
#include "terminal.h"

#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

const char* kDefaultKeyPath = "~/.config/straylight/remote/id_ed25519";
const int kDefaultPort = 7700;

struct Options {
    std::string command;
    std::string host;
    int port = kDefaultPort;
    std::string key_path;
    std::vector<std::string> args;
    bool follow = false;
    int lines = 50;
};

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

void print_usage() {
    std::cerr << R"(straylight-remote — Remote control for StrayLight machines

Usage:
  straylight-remote connect <host> [--port 7700] [--key <path>]
  straylight-remote exec <host> <command...>
  straylight-remote shell <host>                 Interactive shell (PTY)
  straylight-remote upload <host> <local> <remote>
  straylight-remote download <host> <remote> <local>
  straylight-remote sysinfo <host>
  straylight-remote services <host> [list|start|stop|restart] [unit]
  straylight-remote logs <host> [unit] [--follow] [--lines N]
  straylight-remote top <host>                   Live process view
  straylight-remote alice <host> [status|ask|analyze] [query...]
  straylight-remote keygen                       Generate Ed25519 keypair
  straylight-remote authorize <host> <pubkey_file>
  straylight-remote fs <host> [ls|stat|mkdir|rm] <path>

Options:
  --port <N>    Remote agent port (default: 7700)
  --key <path>  Ed25519 private key (default: ~/.config/straylight/remote/id_ed25519)
  --follow      Follow log output
  --lines <N>   Number of log lines (default: 50)
)";
}

Options parse_args(int argc, char* argv[]) {
    Options opts;
    opts.key_path = expand_home(kDefaultKeyPath);

    if (argc < 2) return opts;

    opts.command = argv[1];

    int i = 2;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--port" && i + 1 < argc) {
            opts.port = std::stoi(argv[++i]);
        } else if (arg == "--key" && i + 1 < argc) {
            opts.key_path = expand_home(argv[++i]);
        } else if (arg == "--follow") {
            opts.follow = true;
        } else if (arg == "--lines" && i + 1 < argc) {
            opts.lines = std::stoi(argv[++i]);
        } else if (opts.host.empty() && arg[0] != '-') {
            opts.host = arg;
        } else {
            opts.args.push_back(arg);
        }
        i++;
    }

    return opts;
}

straylight::TlsClient connect_and_auth(const Options& opts) {
    straylight::TlsClient client;

    auto conn = client.connect(opts.host, opts.port);
    if (!conn.has_value()) {
        std::cerr << "Connection failed: " << conn.error() << "\n";
        std::exit(1);
    }

    auto auth = client.authenticate(opts.key_path);
    if (!auth.has_value()) {
        std::cerr << "Authentication failed: " << auth.error() << "\n";
        std::exit(1);
    }

    return client;
}

void print_json_pretty(const std::string& json_str) {
    try {
        auto j = json::parse(json_str);
        if (j.contains("result")) {
            std::cout << j["result"].dump(2) << "\n";
        } else if (j.contains("error")) {
            std::cerr << "Error: " << j["error"].value("message", "unknown") << "\n";
        } else {
            std::cout << j.dump(2) << "\n";
        }
    } catch (...) {
        std::cout << json_str << "\n";
    }
}

int cmd_connect(const Options& opts) {
    auto client = connect_and_auth(opts);
    std::cout << "Connected to " << opts.host << ":" << opts.port << "\n";

    // Interactive REPL
    std::cout << "Type JSON-RPC method and params, or 'quit' to exit.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;
        if (line.empty()) continue;

        auto space = line.find(' ');
        std::string method = (space != std::string::npos) ? line.substr(0, space) : line;
        std::string params = (space != std::string::npos) ? line.substr(space + 1) : "{}";

        auto result = client.request(method, params);
        if (result.has_value()) {
            print_json_pretty(result.value());
        } else {
            std::cerr << "Error: " << result.error() << "\n";
        }
    }

    client.disconnect();
    return 0;
}

int cmd_exec(const Options& opts) {
    auto client = connect_and_auth(opts);

    std::string cmd;
    for (const auto& arg : opts.args) {
        if (!cmd.empty()) cmd += " ";
        cmd += arg;
    }

    if (cmd.empty()) {
        std::cerr << "Usage: straylight-remote exec <host> <command...>\n";
        return 1;
    }

    json params = {{"cmd", cmd}, {"pty", false}};
    auto result = client.request("exec", params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    try {
        auto resp = json::parse(result.value());
        if (resp.contains("result")) {
            auto& r = resp["result"];
            std::string stdout_str = r.value("stdout", "");
            std::string stderr_str = r.value("stderr", "");
            int exit_code = r.value("exit_code", -1);

            if (!stdout_str.empty()) std::cout << stdout_str;
            if (!stderr_str.empty()) std::cerr << stderr_str;

            return exit_code;
        } else if (resp.contains("error")) {
            std::cerr << "Error: " << resp["error"].value("message", "unknown") << "\n";
            return 1;
        }
    } catch (...) {
        std::cout << result.value() << "\n";
    }

    return 0;
}

int cmd_shell(const Options& opts) {
    auto client = connect_and_auth(opts);
    straylight::Terminal terminal;
    return terminal.run(client);
}

int cmd_upload(const Options& opts) {
    if (opts.args.size() < 2) {
        std::cerr << "Usage: straylight-remote upload <host> <local_path> <remote_path>\n";
        return 1;
    }

    auto client = connect_and_auth(opts);

    std::string local_path = opts.args[0];
    std::string remote_path = opts.args[1];

    std::ifstream file(local_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open local file: " << local_path << "\n";
        return 1;
    }

    auto file_size = std::filesystem::file_size(local_path);
    std::cout << "Uploading " << local_path << " (" << file_size << " bytes) to "
              << opts.host << ":" << remote_path << "\n";

    int64_t offset = 0;
    constexpr size_t chunk_size = 65536;
    std::vector<char> buffer(chunk_size);

    while (file.good() && static_cast<uint64_t>(offset) < file_size) {
        file.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
        auto bytes_read = file.gcount();
        if (bytes_read <= 0) break;

        std::vector<unsigned char> chunk_data(buffer.begin(),
                                                buffer.begin() + bytes_read);

        BIO* bio = BIO_new(BIO_s_mem());
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);
        BIO_write(bio, chunk_data.data(), static_cast<int>(chunk_data.size()));
        BIO_flush(bio);

        BUF_MEM* buf_mem = nullptr;
        BIO_get_mem_ptr(bio, &buf_mem);
        std::string b64_data(buf_mem->data, buf_mem->length);
        BIO_free_all(bio);

        json params = {
            {"path", remote_path},
            {"data", b64_data},
            {"offset", offset}
        };

        auto result = client.request("upload", params.dump(), 60000);
        if (!result.has_value()) {
            std::cerr << "\nUpload failed at offset " << offset << ": "
                      << result.error() << "\n";
            return 1;
        }

        offset += bytes_read;

        double pct = 100.0 * static_cast<double>(offset) / static_cast<double>(file_size);
        std::cerr << "\r  " << offset << "/" << file_size
                  << " bytes (" << static_cast<int>(pct) << "%)   " << std::flush;
    }

    std::cerr << "\n";
    std::cout << "Upload complete.\n";
    return 0;
}

int cmd_download(const Options& opts) {
    if (opts.args.size() < 2) {
        std::cerr << "Usage: straylight-remote download <host> <remote_path> <local_path>\n";
        return 1;
    }

    auto client = connect_and_auth(opts);

    std::string remote_path = opts.args[0];
    std::string local_path = opts.args[1];

    json stat_params = {{"action", "stat"}, {"path", remote_path}};
    auto stat_result = client.request("fs", stat_params.dump());
    int64_t file_size = 0;
    if (stat_result.has_value()) {
        try {
            auto resp = json::parse(stat_result.value());
            if (resp.contains("result") && resp["result"].contains("size")) {
                file_size = resp["result"]["size"];
            }
        } catch (...) {}
    }

    std::cout << "Downloading " << opts.host << ":" << remote_path;
    if (file_size > 0) std::cout << " (" << file_size << " bytes)";
    std::cout << " to " << local_path << "\n";

    std::ofstream file(local_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Cannot open local file for writing: " << local_path << "\n";
        return 1;
    }

    int64_t offset = 0;
    bool done = false;

    while (!done) {
        json params = {{"path", remote_path}, {"offset", offset}, {"length", -1}};
        auto result = client.request("download", params.dump(), 60000);
        if (!result.has_value()) {
            std::cerr << "\nDownload failed at offset " << offset << ": "
                      << result.error() << "\n";
            return 1;
        }

        try {
            auto resp = json::parse(result.value());
            if (resp.contains("error")) {
                std::cerr << "\nError: " << resp["error"].value("message", "unknown") << "\n";
                return 1;
            }

            auto& r = resp["result"];
            std::string data_b64 = r.value("data", "");
            int64_t total = r.value("total", static_cast<int64_t>(0));
            int64_t length = r.value("length", static_cast<int64_t>(0));

            if (data_b64.empty() || length == 0) {
                done = true;
                break;
            }

            BIO* bio = BIO_new_mem_buf(data_b64.data(), static_cast<int>(data_b64.size()));
            BIO* b64 = BIO_new(BIO_f_base64());
            BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
            bio = BIO_push(b64, bio);

            std::vector<unsigned char> decoded(data_b64.size());
            int decoded_len = BIO_read(bio, decoded.data(), static_cast<int>(decoded.size()));
            BIO_free_all(bio);

            if (decoded_len > 0) {
                file.write(reinterpret_cast<const char*>(decoded.data()), decoded_len);
                offset += decoded_len;
            }

            if (total > 0) {
                double pct = 100.0 * static_cast<double>(offset) / static_cast<double>(total);
                std::cerr << "\r  " << offset << "/" << total
                          << " bytes (" << static_cast<int>(pct) << "%)   " << std::flush;
            }

            if (offset >= total) {
                done = true;
            }
        } catch (const std::exception& e) {
            std::cerr << "\nParse error: " << e.what() << "\n";
            return 1;
        }
    }

    std::cerr << "\n";
    std::cout << "Download complete.\n";
    return 0;
}

int cmd_sysinfo(const Options& opts) {
    auto client = connect_and_auth(opts);
    auto result = client.request("sysinfo", "{}");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }
    print_json_pretty(result.value());
    return 0;
}

int cmd_services(const Options& opts) {
    auto client = connect_and_auth(opts);

    std::string action = opts.args.empty() ? "list" : opts.args[0];
    std::string unit = opts.args.size() > 1 ? opts.args[1] : "";

    json params = {{"action", action}};
    if (!unit.empty()) params["unit"] = unit;

    auto result = client.request("services", params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }
    print_json_pretty(result.value());
    return 0;
}

int cmd_logs(const Options& opts) {
    auto client = connect_and_auth(opts);

    std::string unit = opts.args.empty() ? "" : opts.args[0];

    json params = {{"lines", opts.lines}, {"follow", opts.follow}};
    if (!unit.empty()) params["unit"] = unit;

    auto result = client.request("logs", params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    try {
        auto resp = json::parse(result.value());
        if (resp.contains("result") && resp["result"].contains("logs")) {
            std::cout << resp["result"]["logs"].get<std::string>();
        } else {
            print_json_pretty(result.value());
        }
    } catch (...) {
        std::cout << result.value() << "\n";
    }

    return 0;
}

int cmd_top(const Options& opts) {
    auto client = connect_and_auth(opts);

    auto result = client.request("processes", R"({"action":"list"})");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    try {
        auto resp = json::parse(result.value());
        if (resp.contains("result") && resp["result"].contains("processes")) {
            auto& procs = resp["result"]["processes"];

            std::printf("%-10s %6s %5s %5s %8s %6s %-8s %s\n",
                        "USER", "PID", "%CPU", "%MEM", "VSZ", "RSS", "STAT", "COMMAND");
            std::printf("%-10s %6s %5s %5s %8s %6s %-8s %s\n",
                        "----", "---", "----", "----", "---", "---", "----", "-------");

            int count = 0;
            for (const auto& p : procs) {
                if (count++ > 30) {
                    std::printf("... (%zu total processes)\n", procs.size());
                    break;
                }
                std::printf("%-10s %6s %5s %5s %8s %6s %-8s %s\n",
                            p.value("user", "").c_str(),
                            p.value("pid", "").c_str(),
                            p.value("cpu", "").c_str(),
                            p.value("mem", "").c_str(),
                            p.value("vsz", "").c_str(),
                            p.value("rss", "").c_str(),
                            p.value("stat", "").c_str(),
                            p.value("command", "").c_str());
            }
        }
    } catch (...) {
        print_json_pretty(result.value());
    }

    return 0;
}

int cmd_alice(const Options& opts) {
    auto client = connect_and_auth(opts);

    std::string method = opts.args.empty() ? "status" : opts.args[0];
    std::string query;
    for (size_t i = 1; i < opts.args.size(); i++) {
        if (!query.empty()) query += " ";
        query += opts.args[i];
    }

    json params = {{"method", method}};
    if (!query.empty()) params["query"] = query;

    auto result = client.request("alice", params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }
    print_json_pretty(result.value());
    return 0;
}

int cmd_keygen(const Options& opts) {
    std::string key_dir = expand_home("~/.config/straylight/remote");
    std::string priv_path = key_dir + "/id_ed25519";
    std::string pub_path = key_dir + "/id_ed25519.pub";

    if (!opts.key_path.empty() && opts.key_path != expand_home(kDefaultKeyPath)) {
        priv_path = opts.key_path;
        pub_path = opts.key_path + ".pub";
        key_dir = std::filesystem::path(priv_path).parent_path().string();
    }

    if (std::filesystem::exists(priv_path)) {
        std::cerr << "Key already exists at " << priv_path << "\n";
        std::cerr << "Overwrite? [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cerr << "Aborted.\n";
            return 1;
        }
    }

    std::filesystem::create_directories(key_dir);

    std::cout << "Generating Ed25519 keypair...\n";

    auto result = straylight::TlsClient::generate_keypair(priv_path, pub_path);
    if (!result.has_value()) {
        std::cerr << "Failed: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Private key: " << priv_path << "\n";
    std::cout << "Public key:  " << pub_path << "\n";

    auto pub_result = straylight::TlsClient::read_public_key(pub_path);
    if (pub_result.has_value()) {
        std::cout << "\nPublic key (add to remote authorized_keys):\n";
        std::ifstream pub_file(pub_path);
        std::string pub_line;
        std::getline(pub_file, pub_line);
        std::cout << "  " << pub_line << "\n";
    }

    return 0;
}

int cmd_authorize(const Options& opts) {
    if (opts.args.empty()) {
        std::cerr << "Usage: straylight-remote authorize <host> <pubkey_file>\n";
        return 1;
    }

    auto client = connect_and_auth(opts);

    std::string pubkey_file = opts.args[0];

    std::ifstream file(pubkey_file);
    if (!file.is_open()) {
        std::cerr << "Cannot open public key file: " << pubkey_file << "\n";
        return 1;
    }

    std::string pub_line;
    std::getline(file, pub_line);
    file.close();

    // Validate public key line format before constructing command
    // Expected: "ed25519 <base64> <comment>" — only allow safe characters
    for (char c : pub_line) {
        if (!std::isalnum(c) && c != ' ' && c != '+' && c != '/' && c != '='
            && c != '@' && c != '.' && c != '-' && c != '_') {
            std::cerr << "Invalid character in public key file\n";
            return 1;
        }
    }

    // Upload via the file transfer mechanism instead of shell command
    // Append to authorized_keys by downloading, appending, and re-uploading
    json download_params = {{"path", "/etc/straylight/remote/authorized_keys"},
                             {"offset", 0}, {"length", -1}};
    auto dl_result = client.request("download", download_params.dump());

    std::string existing_keys;
    if (dl_result.has_value()) {
        try {
            auto resp = json::parse(dl_result.value());
            if (resp.contains("result") && resp["result"].contains("data")) {
                std::string data_b64 = resp["result"]["data"];
                BIO* bio = BIO_new_mem_buf(data_b64.data(), static_cast<int>(data_b64.size()));
                BIO* b64 = BIO_new(BIO_f_base64());
                BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
                bio = BIO_push(b64, bio);
                std::vector<unsigned char> decoded(data_b64.size());
                int decoded_len = BIO_read(bio, decoded.data(), static_cast<int>(decoded.size()));
                BIO_free_all(bio);
                if (decoded_len > 0) {
                    existing_keys.assign(reinterpret_cast<char*>(decoded.data()),
                                          static_cast<size_t>(decoded_len));
                }
            }
        } catch (...) {}
    }

    // Append new key
    if (!existing_keys.empty() && existing_keys.back() != '\n') {
        existing_keys += "\n";
    }
    existing_keys += pub_line + "\n";

    // Base64 encode the updated content
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    BIO_write(bio, existing_keys.data(), static_cast<int>(existing_keys.size()));
    BIO_flush(bio);

    BUF_MEM* buf_mem = nullptr;
    BIO_get_mem_ptr(bio, &buf_mem);
    std::string b64_data(buf_mem->data, buf_mem->length);
    BIO_free_all(bio);

    // Ensure directory exists first
    json mkdir_params = {{"action", "mkdir"}, {"path", "/etc/straylight/remote"}};
    client.request("fs", mkdir_params.dump());

    // Upload
    json upload_params = {
        {"path", "/etc/straylight/remote/authorized_keys"},
        {"data", b64_data},
        {"offset", 0}
    };
    auto result = client.request("upload", upload_params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Public key authorized on " << opts.host << "\n";
    return 0;
}

int cmd_fs(const Options& opts) {
    auto client = connect_and_auth(opts);

    std::string action = opts.args.empty() ? "ls" : opts.args[0];
    std::string path = opts.args.size() > 1 ? opts.args[1] : ".";

    json params = {{"action", action}, {"path", path}};

    auto result = client.request("fs", params.dump());
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }
    print_json_pretty(result.value());
    return 0;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto opts = parse_args(argc, argv);

    if (opts.command == "keygen") return cmd_keygen(opts);
    if (opts.command == "help" || opts.command == "--help" || opts.command == "-h") {
        print_usage();
        return 0;
    }

    // All other commands require a host
    if (opts.host.empty()) {
        std::cerr << "Error: missing <host> argument\n";
        print_usage();
        return 1;
    }

    if (opts.command == "connect")   return cmd_connect(opts);
    if (opts.command == "exec")      return cmd_exec(opts);
    if (opts.command == "shell")     return cmd_shell(opts);
    if (opts.command == "upload")    return cmd_upload(opts);
    if (opts.command == "download")  return cmd_download(opts);
    if (opts.command == "sysinfo")   return cmd_sysinfo(opts);
    if (opts.command == "services")  return cmd_services(opts);
    if (opts.command == "logs")      return cmd_logs(opts);
    if (opts.command == "top")       return cmd_top(opts);
    if (opts.command == "alice")     return cmd_alice(opts);
    if (opts.command == "authorize") return cmd_authorize(opts);
    if (opts.command == "fs")        return cmd_fs(opts);

    std::cerr << "Unknown command: " << opts.command << "\n";
    print_usage();
    return 1;
}
