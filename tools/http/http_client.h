// tools/http/http_client.h
// HTTP client/tester for StrayLight OS — requests, timing, collections.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Timing breakdown for an HTTP request.
struct HttpTiming {
    double dns_ms = 0;
    double connect_ms = 0;
    double tls_ms = 0;
    double first_byte_ms = 0;
    double transfer_ms = 0;
    double total_ms = 0;
};

/// HTTP response.
struct HttpResponse {
    int         status_code = 0;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
    size_t      content_length = 0;
    std::string content_type;
    HttpTiming  timing;
};

/// Saved request in a collection.
struct SavedRequest {
    std::string name;
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string auth_type;   // "basic", "bearer", ""
    std::string auth_value;
};

/// HTTP request configuration.
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string auth_type;
    std::string auth_value;
    bool        follow_redirects = true;
    int         max_redirects = 10;
    int         timeout_secs = 30;
    std::string proxy;
    bool        verbose = false;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    /// Execute an HTTP request.
    Result<HttpResponse, std::string> request(const HttpRequest& req) const;

    /// Convenience methods.
    Result<HttpResponse, std::string> get(const std::string& url,
                                           const std::map<std::string, std::string>& headers = {}) const;
    Result<HttpResponse, std::string> post(const std::string& url, const std::string& data,
                                            const std::map<std::string, std::string>& headers = {}) const;
    Result<HttpResponse, std::string> put(const std::string& url, const std::string& data,
                                           const std::map<std::string, std::string>& headers = {}) const;
    Result<HttpResponse, std::string> del(const std::string& url,
                                           const std::map<std::string, std::string>& headers = {}) const;

    /// Timing analysis — performs request and returns detailed timing.
    Result<HttpTiming, std::string> time_request(const std::string& url) const;

    /// Save a request to a named collection.
    Result<void, std::string> save_request(const SavedRequest& req) const;

    /// List all saved collections.
    Result<std::vector<SavedRequest>, std::string> list_collection() const;

    /// Run all requests in a collection.
    Result<std::vector<std::pair<std::string, HttpResponse>>, std::string> run_collection() const;

private:
    std::string collections_dir() const;
    std::string cookie_jar_path() const;
    std::string build_curl_cmd(const HttpRequest& req) const;
    Result<HttpResponse, std::string> execute_curl(const std::string& cmd) const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    HttpResponse parse_curl_output(const std::string& header_data,
                                    const std::string& body_data) const;
};

} // namespace straylight
