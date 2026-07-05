// tools/neurolinkd-cli/main.cpp
// Low-latency helper for calling neurolinkd without shell JSON quoting.

#include "tools/http/http_client.h"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>

static void usage() {
    std::cerr
        << "straylight-neurolinkd — neurolinkd helper CLI\n\n"
        << "Usage:\n"
        << "  straylight-neurolinkd health [--base=URL]\n"
        << "  straylight-neurolinkd intent <INTENT> [--strength=F] [--confidence=F] [--source=raw|decoded|sim] [--base=URL]\n"
        << "  straylight-neurolinkd cancel [--base=URL]\n"
        << "  straylight-neurolinkd propose <ACTION> [--risk=low|med|high] [--target=T] [--base=URL]\n"
        << "\n"
        << "Defaults:\n"
        << "  base: http://127.0.0.1:8790\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return "";
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Drop other control chars.
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static bool valid_enum_token(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

static int http_json_post(
    straylight::HttpClient& client,
    const std::string& url,
    const std::string& body) {
    straylight::HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.headers = {{"content-type", "application/json"}};
    req.body = body;
    req.follow_redirects = false;
    req.verbose = false;

    auto res = client.request(req);
    if (!res.has_value()) {
        std::cerr << "Error: " << res.error() << "\n";
        return 1;
    }
    std::cout << res.value().body;
    if (!res.value().body.empty() && res.value().body.back() != '\n') std::cout << "\n";
    return (res.value().status_code >= 200 && res.value().status_code < 300) ? 0 : 2;
}

static int http_get(
    straylight::HttpClient& client,
    const std::string& url) {
    straylight::HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.follow_redirects = false;
    req.verbose = false;

    auto res = client.request(req);
    if (!res.has_value()) {
        std::cerr << "Error: " << res.error() << "\n";
        return 1;
    }
    std::cout << res.value().body;
    if (!res.value().body.empty() && res.value().body.back() != '\n') std::cout << "\n";
    return (res.value().status_code >= 200 && res.value().status_code < 300) ? 0 : 2;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    std::string base = get_arg(argc, argv, "--base=");
    if (base.empty()) base = "http://127.0.0.1:8790";

    std::string cmd = argv[1];
    straylight::HttpClient client;

    if (cmd == "health") {
        return http_get(client, base + "/healthz");
    }

    if (cmd == "intent") {
        if (argc < 3) {
            std::cerr << "Error: intent requires INTENT\n";
            return 1;
        }
        std::string intent = argv[2];
        if (!valid_enum_token(intent)) {
            std::cerr << "Error: invalid INTENT token\n";
            return 1;
        }
        std::string source = get_arg(argc, argv, "--source=");
        if (source.empty()) source = "sim";
        if (!(source == "raw" || source == "decoded" || source == "sim")) {
            std::cerr << "Error: invalid --source\n";
            return 1;
        }
        std::string strength = get_arg(argc, argv, "--strength=");
        if (strength.empty()) strength = "0.8";
        std::string confidence = get_arg(argc, argv, "--confidence=");
        if (confidence.empty()) confidence = "0.7";

        std::string body =
            std::string("{\"intent\":\"") + json_escape(intent) +
            "\",\"source\":\"" + json_escape(source) +
            "\",\"strength\":" + strength +
            ",\"confidence\":" + confidence +
            "}";

        return http_json_post(client, base + "/intent", body);
    }

    if (cmd == "cancel") {
        std::string body = "{\"intent\":\"CANCEL\",\"source\":\"sim\",\"strength\":1.0,\"confidence\":1.0}";
        return http_json_post(client, base + "/intent", body);
    }

    if (cmd == "propose") {
        if (argc < 3) {
            std::cerr << "Error: propose requires ACTION\n";
            return 1;
        }
        std::string action = argv[2];
        if (!valid_enum_token(action)) {
            std::cerr << "Error: invalid ACTION token\n";
            return 1;
        }
        std::string risk = get_arg(argc, argv, "--risk=");
        if (risk.empty()) risk = "low";
        if (!(risk == "low" || risk == "med" || risk == "high")) {
            std::cerr << "Error: invalid --risk\n";
            return 1;
        }
        std::string target = get_arg(argc, argv, "--target=");

        std::string body =
            std::string("{\"action\":\"") + json_escape(action) +
            "\",\"risk\":\"" + json_escape(risk) + "\"";
        if (!target.empty()) {
            body += std::string(",\"target\":\"") + json_escape(target) + "\"";
        }
        body += "}";

        return http_json_post(client, base + "/action/propose", body);
    }

    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage();
        return 0;
    }

    std::cerr << "Error: unknown command\n";
    usage();
    return 1;
}
