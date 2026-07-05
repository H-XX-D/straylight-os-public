// tools/http/http_client.cpp
// Full HTTP client implementation for StrayLight OS.

#include "http_client.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

HttpClient::HttpClient() {
    fs::create_directories(collections_dir());
}

HttpClient::~HttpClient() = default;

std::string HttpClient::collections_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/http/collections";
}

std::string HttpClient::cookie_jar_path() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/http/cookies.txt";
}

Result<std::string, std::string> HttpClient::run_cmd(const std::string& cmd) const {
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
    // curl returns non-zero for HTTP errors, but we still want the output
    if (rc != 0 && output.empty()) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

// ---------------------------------------------------------------------------
// Build curl command
// ---------------------------------------------------------------------------

std::string HttpClient::build_curl_cmd(const HttpRequest& req) const {
    std::ostringstream cmd;
    cmd << "curl -s -S -w '\\n---STRAYLIGHT_TIMING---\\n"
        << "time_namelookup: %{time_namelookup}\\n"
        << "time_connect: %{time_connect}\\n"
        << "time_appconnect: %{time_appconnect}\\n"
        << "time_starttransfer: %{time_starttransfer}\\n"
        << "time_total: %{time_total}\\n"
        << "http_code: %{http_code}\\n"
        << "size_download: %{size_download}\\n"
        << "content_type: %{content_type}\\n"
        << "' -D -";  // dump headers to stdout

    cmd << " -X " << req.method;

    if (req.follow_redirects) {
        cmd << " -L --max-redirs " << req.max_redirects;
    }

    cmd << " --max-time " << req.timeout_secs;

    // Cookie jar
    cmd << " -b '" << cookie_jar_path() << "'"
        << " -c '" << cookie_jar_path() << "'";

    // Headers
    for (const auto& [key, value] : req.headers) {
        cmd << " -H '" << key << ": " << value << "'";
    }

    // Auth
    if (req.auth_type == "basic") {
        cmd << " -u '" << req.auth_value << "'";
    } else if (req.auth_type == "bearer") {
        cmd << " -H 'Authorization: Bearer " << req.auth_value << "'";
    }

    // Body
    if (!req.body.empty()) {
        cmd << " -d '" << req.body << "'";
        // Auto-set Content-Type if not specified
        if (req.headers.find("Content-Type") == req.headers.end()) {
            if (req.body.front() == '{' || req.body.front() == '[') {
                cmd << " -H 'Content-Type: application/json'";
            }
        }
    }

    // Proxy
    if (!req.proxy.empty()) {
        cmd << " -x '" << req.proxy << "'";
    }

    if (req.verbose) {
        cmd << " -v";
    }

    cmd << " '" << req.url << "' 2>&1";

    return cmd.str();
}

// ---------------------------------------------------------------------------
// Parse response
// ---------------------------------------------------------------------------

HttpResponse HttpClient::parse_curl_output(const std::string& header_data,
                                             const std::string& body_data) const {
    HttpResponse resp;

    // Parse headers
    std::istringstream hstream(header_data);
    std::string line;
    bool first_line = true;
    while (std::getline(hstream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (first_line || line.rfind("HTTP/", 0) == 0) {
            // Status line: HTTP/1.1 200 OK
            std::regex status_re(R"(HTTP/\S+\s+(\d+)\s*(.*))");
            std::smatch m;
            if (std::regex_search(line, m, status_re)) {
                resp.status_code = std::stoi(m[1].str());
                resp.status_text = m[2].str();
            }
            first_line = false;
        } else {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                auto pos = val.find_first_not_of(" \t");
                if (pos != std::string::npos) val = val.substr(pos);
                resp.headers[key] = val;
            }
        }
    }

    resp.body = body_data;
    return resp;
}

// ---------------------------------------------------------------------------
// Execute curl and parse
// ---------------------------------------------------------------------------

Result<HttpResponse, std::string> HttpClient::execute_curl(const std::string& cmd) const {
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<HttpResponse, std::string>::error(res.error());
    }

    std::string output = res.value();

    // Split on our timing marker
    auto marker_pos = output.find("---STRAYLIGHT_TIMING---");
    std::string main_output = (marker_pos != std::string::npos) ?
        output.substr(0, marker_pos) : output;
    std::string timing_data = (marker_pos != std::string::npos) ?
        output.substr(marker_pos + 23) : "";

    // Split main output into headers and body
    // curl -D - puts headers first, then a blank line, then body
    std::string header_data, body_data;
    bool found_body = false;
    size_t pos = 0;
    size_t last_header_end = 0;

    // Find the last occurrence of HTTP status line to handle redirects
    size_t search_pos = 0;
    while (true) {
        auto http_pos = main_output.find("HTTP/", search_pos);
        if (http_pos == std::string::npos) break;

        // Find the blank line after this header block
        auto blank = main_output.find("\r\n\r\n", http_pos);
        if (blank == std::string::npos) blank = main_output.find("\n\n", http_pos);
        if (blank != std::string::npos) {
            last_header_end = blank;
            search_pos = blank + 2;
        } else {
            break;
        }
    }

    if (last_header_end > 0) {
        header_data = main_output.substr(0, last_header_end);
        size_t body_start = last_header_end;
        while (body_start < main_output.size() &&
               (main_output[body_start] == '\r' || main_output[body_start] == '\n')) {
            ++body_start;
        }
        body_data = main_output.substr(body_start);
    } else {
        body_data = main_output;
    }

    HttpResponse resp = parse_curl_output(header_data, body_data);

    // Parse timing
    if (!timing_data.empty()) {
        std::istringstream ts(timing_data);
        std::string line;
        double t_dns = 0, t_connect = 0, t_tls = 0, t_first = 0, t_total = 0;

        while (std::getline(ts, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            auto vpos = val.find_first_not_of(" \t");
            if (vpos != std::string::npos) val = val.substr(vpos);

            try {
                if (key == "time_namelookup") t_dns = std::stod(val);
                else if (key == "time_connect") t_connect = std::stod(val);
                else if (key == "time_appconnect") t_tls = std::stod(val);
                else if (key == "time_starttransfer") t_first = std::stod(val);
                else if (key == "time_total") t_total = std::stod(val);
                else if (key == "http_code") resp.status_code = std::stoi(val);
                else if (key == "size_download") resp.content_length = std::stoull(val);
                else if (key == "content_type") resp.content_type = val;
            } catch (...) {}
        }

        resp.timing.dns_ms = t_dns * 1000.0;
        resp.timing.connect_ms = (t_connect - t_dns) * 1000.0;
        resp.timing.tls_ms = (t_tls > t_connect) ? (t_tls - t_connect) * 1000.0 : 0;
        resp.timing.first_byte_ms = (t_first - t_tls) * 1000.0;
        resp.timing.transfer_ms = (t_total - t_first) * 1000.0;
        resp.timing.total_ms = t_total * 1000.0;
    }

    return Result<HttpResponse, std::string>::ok(resp);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<HttpResponse, std::string> HttpClient::request(const HttpRequest& req) const {
    std::string cmd = build_curl_cmd(req);
    return execute_curl(cmd);
}

Result<HttpResponse, std::string> HttpClient::get(const std::string& url,
                                                    const std::map<std::string, std::string>& headers) const {
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = headers;
    return request(req);
}

Result<HttpResponse, std::string> HttpClient::post(const std::string& url,
                                                     const std::string& data,
                                                     const std::map<std::string, std::string>& headers) const {
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.body = data;
    req.headers = headers;
    return request(req);
}

Result<HttpResponse, std::string> HttpClient::put(const std::string& url,
                                                    const std::string& data,
                                                    const std::map<std::string, std::string>& headers) const {
    HttpRequest req;
    req.method = "PUT";
    req.url = url;
    req.body = data;
    req.headers = headers;
    return request(req);
}

Result<HttpResponse, std::string> HttpClient::del(const std::string& url,
                                                    const std::map<std::string, std::string>& headers) const {
    HttpRequest req;
    req.method = "DELETE";
    req.url = url;
    req.headers = headers;
    return request(req);
}

Result<HttpTiming, std::string> HttpClient::time_request(const std::string& url) const {
    auto res = get(url);
    if (!res.has_value()) {
        return Result<HttpTiming, std::string>::error(res.error());
    }
    return Result<HttpTiming, std::string>::ok(res.value().timing);
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

Result<void, std::string> HttpClient::save_request(const SavedRequest& req) const {
    std::string path = collections_dir() + "/" + req.name + ".req";
    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to " + path);
    }

    out << "method=" << req.method << "\n"
        << "url=" << req.url << "\n";

    for (const auto& [key, value] : req.headers) {
        out << "header=" << key << ": " << value << "\n";
    }

    if (!req.body.empty()) out << "body=" << req.body << "\n";
    if (!req.auth_type.empty()) out << "auth_type=" << req.auth_type << "\n";
    if (!req.auth_value.empty()) out << "auth_value=" << req.auth_value << "\n";

    return Result<void, std::string>::ok();
}

Result<std::vector<SavedRequest>, std::string> HttpClient::list_collection() const {
    std::vector<SavedRequest> requests;

    if (!fs::exists(collections_dir())) {
        return Result<std::vector<SavedRequest>, std::string>::ok(requests);
    }

    for (const auto& entry : fs::directory_iterator(collections_dir())) {
        std::string fname = entry.path().filename().string();
        if (fname.size() <= 4 || fname.substr(fname.size() - 4) != ".req") continue;

        SavedRequest req;
        req.name = fname.substr(0, fname.size() - 4);

        std::ifstream in(entry.path().string());
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "method") req.method = val;
            else if (key == "url") req.url = val;
            else if (key == "body") req.body = val;
            else if (key == "auth_type") req.auth_type = val;
            else if (key == "auth_value") req.auth_value = val;
            else if (key == "header") {
                auto colon = val.find(':');
                if (colon != std::string::npos) {
                    std::string hkey = val.substr(0, colon);
                    std::string hval = val.substr(colon + 1);
                    auto pos = hval.find_first_not_of(" \t");
                    if (pos != std::string::npos) hval = hval.substr(pos);
                    req.headers[hkey] = hval;
                }
            }
        }

        requests.push_back(req);
    }

    return Result<std::vector<SavedRequest>, std::string>::ok(requests);
}

Result<std::vector<std::pair<std::string, HttpResponse>>, std::string>
HttpClient::run_collection() const {
    auto list_res = list_collection();
    if (!list_res.has_value()) {
        return Result<std::vector<std::pair<std::string, HttpResponse>>, std::string>::error(
            list_res.error());
    }

    std::vector<std::pair<std::string, HttpResponse>> results;

    for (const auto& saved : list_res.value()) {
        HttpRequest req;
        req.method = saved.method;
        req.url = saved.url;
        req.headers = saved.headers;
        req.body = saved.body;
        req.auth_type = saved.auth_type;
        req.auth_value = saved.auth_value;

        auto res = request(req);
        if (res.has_value()) {
            results.emplace_back(saved.name, res.value());
        } else {
            HttpResponse err_resp;
            err_resp.status_code = -1;
            err_resp.status_text = res.error();
            results.emplace_back(saved.name, err_resp);
        }
    }

    return Result<std::vector<std::pair<std::string, HttpResponse>>, std::string>::ok(results);
}

} // namespace straylight
