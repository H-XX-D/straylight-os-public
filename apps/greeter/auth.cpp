// apps/greeter/auth.cpp
// PAM authentication implementation
#include "auth.h"

#include <straylight/log.h>

#include <security/pam_appl.h>
#include <cstring>
#include <thread>
#include <chrono>

namespace straylight::greeter {

// PAM conversation callback — supplies username and password
struct PamConvData {
    std::string username;
    std::string password;
};

static int pam_conversation(int num_msg,
                            const struct pam_message** msg,
                            struct pam_response** resp,
                            void* appdata_ptr) {
    auto* data = static_cast<PamConvData*>(appdata_ptr);
    if (!data || num_msg <= 0) return PAM_CONV_ERR;

    auto* replies = static_cast<struct pam_response*>(
        calloc(static_cast<size_t>(num_msg), sizeof(struct pam_response)));
    if (!replies) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; ++i) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_ON:
                // Username prompt
                replies[i].resp = strdup(data->username.c_str());
                break;
            case PAM_PROMPT_ECHO_OFF:
                // Password prompt
                replies[i].resp = strdup(data->password.c_str());
                break;
            case PAM_TEXT_INFO:
            case PAM_ERROR_MSG:
                // Informational — log but do nothing
                SL_DEBUG("PAM message: {}", msg[i]->msg);
                replies[i].resp = nullptr;
                break;
            default:
                free(replies);
                return PAM_CONV_ERR;
        }
    }

    *resp = replies;
    return PAM_SUCCESS;
}

PamAuth::PamAuth(std::string_view service)
    : service_(service) {
}

PamAuth::~PamAuth() {
    if (pamh_) {
        pam_end(pamh_, PAM_SUCCESS);
        pamh_ = nullptr;
    }
}

straylight::Result<void, straylight::SLError> PamAuth::authenticate(
    std::string_view username,
    std::string_view password) {

    // Rate limiting: after kBackoffThreshold failures, delay
    if (failure_count_ >= kBackoffThreshold) {
        SL_WARN("PAM rate-limit: {} failures, delaying {}s",
                failure_count_, kBackoffSeconds);
        std::this_thread::sleep_for(
            std::chrono::seconds(kBackoffSeconds));
    }

    // Clean up any prior PAM handle
    if (pamh_) {
        pam_end(pamh_, PAM_SUCCESS);
        pamh_ = nullptr;
    }

    // Set up conversation with credentials
    PamConvData conv_data;
    conv_data.username = std::string(username);
    conv_data.password = std::string(password);

    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = &conv_data,
    };

    SL_INFO("PAM authenticate: user='{}'", username);

    // Start PAM session
    int ret = pam_start(service_.c_str(),
                        std::string(username).c_str(),
                        &conv, &pamh_);
    if (ret != PAM_SUCCESS) {
        SL_ERROR("pam_start failed: {}", pam_strerror(pamh_, ret));
        return straylight::Result<void, straylight::SLError>::error(
            straylight::SLError{
                straylight::SLErrorCode::Internal,
                std::string("pam_start failed: ") + pam_strerror(pamh_, ret)
            });
    }

    // Authenticate
    ret = pam_authenticate(pamh_, 0);
    if (ret != PAM_SUCCESS) {
        ++failure_count_;
        SL_WARN("PAM auth failed for '{}': {} (attempt {})",
                username, pam_strerror(pamh_, ret), failure_count_);
        pam_end(pamh_, ret);
        pamh_ = nullptr;
        return straylight::Result<void, straylight::SLError>::error(
            straylight::SLError{
                straylight::SLErrorCode::PermissionDenied,
                "Authentication failed: incorrect username or password"
            });
    }

    // Account validation
    ret = pam_acct_mgmt(pamh_, 0);
    if (ret != PAM_SUCCESS) {
        ++failure_count_;
        SL_WARN("PAM acct_mgmt failed for '{}': {}",
                username, pam_strerror(pamh_, ret));
        pam_end(pamh_, ret);
        pamh_ = nullptr;
        return straylight::Result<void, straylight::SLError>::error(
            straylight::SLError{
                straylight::SLErrorCode::PermissionDenied,
                "Account validation failed"
            });
    }

    // Success — reset failure count
    failure_count_ = 0;
    SL_INFO("PAM auth succeeded for '{}'", username);

    return straylight::Result<void, straylight::SLError>::ok();
}

} // namespace straylight::greeter
