// tools/http/main.cpp
// CLI front-end for straylight-http — HTTP client/tester.

#include "http_client.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-http — HTTP client CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-http get <url> [--header=K:V]             GET request\n"
        << "  straylight-http post <url> --data=X [--header=K:V]   POST request\n"
        << "  straylight-http put <url> --data=X                   PUT request\n"
        << "  straylight-http delete <url>                         DELETE request\n"
        << "  straylight-http time <url>                           Timing breakdown\n"
        << "  straylight-http save <name> <method> <url>           Save to collection\n"
        << "  straylight-http collection list                      List saved requests\n"
        << "  straylight-http collection run                       Run all saved requests\n"
        << "\n"
        << "Options:\n"
        << "  --header=Key:Value       Add request header\n"
        << "  --data=BODY              Request body\n"
        << "  --auth=basic:user:pass   Basic auth\n"
        << "  --auth=bearer:TOKEN      Bearer token\n"
        << "  --proxy=HOST:PORT        Use proxy\n"
        << "  --no-follow              Don't follow redirects\n"
        << "  --verbose                Show request details\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::map<std::string, std::string> collect_headers(int argc, char* argv[], int start = 3) {
    std::map<std::string, std::string> headers;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--header=", 0) == 0) {
            std::string hdr = arg.substr(9);
            auto colon = hdr.find(':');
            if (colon != std::string::npos) {
                std::string key = hdr.substr(0, colon);
                std::string val = hdr.substr(colon + 1);
                headers[key] = val;
            }
        }
    }
    return headers;
}

static void print_response(const straylight::HttpResponse& resp, bool show_headers = true) {
    // Status line
    std::string color_code;
    if (resp.status_code >= 200 && resp.status_code < 300) color_code = "\033[32m";
    else if (resp.status_code >= 300 && resp.status_code < 400) color_code = "\033[33m";
    else if (resp.status_code >= 400) color_code = "\033[31m";

    std::cout << color_code << "HTTP " << resp.status_code << " " << resp.status_text
              << "\033[0m\n";

    if (show_headers && !resp.headers.empty()) {
        for (const auto& [key, value] : resp.headers) {
            std::cout << "\033[36m" << key << "\033[0m: " << value << "\n";
        }
        std::cout << "\n";
    }

    // Body
    if (!resp.body.empty()) {
        std::cout << resp.body;
        if (resp.body.back() != '\n') std::cout << "\n";
    }

    // Timing summary
    if (resp.timing.total_ms > 0) {
        std::cout << "\n\033[90m"
                  << "Time: " << std::fixed << std::setprecision(0) << resp.timing.total_ms << "ms"
                  << " | Size: " << resp.content_length << " bytes"
                  << "\033[0m\n";
    }
}

static void print_timing(const straylight::HttpTiming& t) {
    auto bar = [](double ms, double total) -> std::string {
        int width = 40;
        int filled = (total > 0) ? static_cast<int>((ms / total) * width) : 0;
        std::string result;
        for (int i = 0; i < width; ++i) result += (i < filled) ? "#" : ".";
        return result;
    };

    double total = t.total_ms;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "DNS Lookup:    " << bar(t.dns_ms, total) << "  "
              << std::setw(8) << t.dns_ms << " ms\n";
    std::cout << "TCP Connect:   " << bar(t.connect_ms, total) << "  "
              << std::setw(8) << t.connect_ms << " ms\n";
    std::cout << "TLS Handshake: " << bar(t.tls_ms, total) << "  "
              << std::setw(8) << t.tls_ms << " ms\n";
    std::cout << "First Byte:    " << bar(t.first_byte_ms, total) << "  "
              << std::setw(8) << t.first_byte_ms << " ms\n";
    std::cout << "Transfer:      " << bar(t.transfer_ms, total) << "  "
              << std::setw(8) << t.transfer_ms << " ms\n";
    std::cout << std::string(60, '-') << "\n";
    std::cout << "Total:         " << std::string(40, ' ') << "  "
              << std::setw(8) << t.total_ms << " ms\n";
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

    straylight::HttpClient client;

    // Build common request from flags
    auto make_request = [&](const std::string& method, int url_idx) -> straylight::HttpRequest {
        straylight::HttpRequest req;
        req.method = method;
        req.url = (url_idx < argc) ? argv[url_idx] : "";
        req.headers = collect_headers(argc, argv, url_idx + 1);
        req.body = get_arg(argc, argv, "--data=", url_idx + 1);
        req.follow_redirects = !has_flag(argc, argv, "--no-follow", url_idx + 1);
        req.verbose = has_flag(argc, argv, "--verbose", url_idx + 1);
        req.proxy = get_arg(argc, argv, "--proxy=", url_idx + 1);

        std::string auth = get_arg(argc, argv, "--auth=", url_idx + 1);
        if (!auth.empty()) {
            if (auth.rfind("basic:", 0) == 0) {
                req.auth_type = "basic";
                req.auth_value = auth.substr(6);
            } else if (auth.rfind("bearer:", 0) == 0) {
                req.auth_type = "bearer";
                req.auth_value = auth.substr(7);
            }
        }

        return req;
    };

    // -----------------------------------------------------------------------
    // get/post/put/delete <url>
    // -----------------------------------------------------------------------
    if (command == "get" || command == "post" || command == "put" || command == "delete") {
        if (argc < 3) {
            std::cerr << "Error: '" << command << "' requires a URL\n";
            return 1;
        }

        std::string method = command;
        for (auto& c : method) c = static_cast<char>(toupper(c));

        auto req = make_request(method, 2);
        auto res = client.request(req);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        print_response(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // time <url>
    // -----------------------------------------------------------------------
    if (command == "time") {
        if (argc < 3) {
            std::cerr << "Error: 'time' requires a URL\n";
            return 1;
        }

        auto req = make_request("GET", 2);
        auto res = client.request(req);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << "Timing for " << argv[2] << "\n\n";
        print_timing(res.value().timing);
        return 0;
    }

    // -----------------------------------------------------------------------
    // save <name> <method> <url>
    // -----------------------------------------------------------------------
    if (command == "save") {
        if (argc < 5) {
            std::cerr << "Error: 'save' requires <name> <method> <url>\n";
            return 1;
        }

        straylight::SavedRequest saved;
        saved.name = argv[2];
        saved.method = argv[3];
        for (auto& c : saved.method) c = static_cast<char>(toupper(c));
        saved.url = argv[4];
        saved.headers = collect_headers(argc, argv, 5);
        saved.body = get_arg(argc, argv, "--data=", 5);

        auto res = client.save_request(saved);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Saved request '" << saved.name << "'\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // collection list/run
    // -----------------------------------------------------------------------
    if (command == "collection") {
        if (argc < 3) {
            std::cerr << "Error: 'collection' requires 'list' or 'run'\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "list") {
            auto res = client.list_collection();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& requests = res.value();
            if (requests.empty()) {
                std::cout << "No saved requests.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(20) << "NAME"
                      << std::setw(8) << "METHOD"
                      << "URL\n";
            std::cout << std::string(60, '-') << "\n";
            for (const auto& r : requests) {
                std::cout << std::left
                          << std::setw(20) << r.name
                          << std::setw(8) << r.method
                          << r.url << "\n";
            }
            return 0;
        }

        if (sub == "run") {
            auto res = client.run_collection();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            for (const auto& [name, resp] : res.value()) {
                std::cout << "\033[1m--- " << name << " ---\033[0m\n";
                print_response(resp, false);
                std::cout << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown collection subcommand '" << sub << "'\n";
        return 1;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
