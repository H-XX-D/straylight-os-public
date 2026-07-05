// apps/greeter/auth.h
// PAM authentication wrapper for the login greeter
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <string>
#include <string_view>

// Forward declaration — the actual PAM types come from <security/pam_appl.h>
struct pam_handle;
typedef struct pam_handle pam_handle_t;

namespace straylight::greeter {

/// PAM-based authentication for the StrayLight greeter.
/// Wraps pam_start / pam_authenticate / pam_acct_mgmt / pam_end.
/// Rate-limits after repeated failures (3s incremental backoff after 3 fails).
class PamAuth {
public:
    /// Create a PAM auth context with the given service name.
    /// The service name maps to /etc/pam.d/<service>.
    explicit PamAuth(std::string_view service = "straylight-greeter");
    ~PamAuth();

    PamAuth(const PamAuth&) = delete;
    PamAuth& operator=(const PamAuth&) = delete;

    /// Authenticate a user with username and password.
    /// Returns ok on success, or an error with SLErrorCode::PermissionDenied
    /// on auth failure. Never logs the password.
    straylight::Result<void, straylight::SLError> authenticate(
        std::string_view username,
        std::string_view password);

    /// Get the number of consecutive failed attempts.
    [[nodiscard]] int failure_count() const { return failure_count_; }

private:
    std::string service_;
    pam_handle_t* pamh_ = nullptr;
    int failure_count_ = 0;

    static constexpr int kBackoffThreshold = 3;
    static constexpr int kBackoffSeconds   = 3;
};

} // namespace straylight::greeter
