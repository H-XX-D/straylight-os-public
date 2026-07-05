#pragma once
// apps/settings/pages/security.h
#include "../settings_page.h"
#include <string>
#include <vector>

namespace straylight::settings {

struct EnclaveInfo {
    bool        sgx_supported;
    bool        sgx_enabled;
    int         enclave_count;
    int         epc_memory_mb;
    std::string platform_sw_version;
    std::string dcap_version;
    float       attestation_success_rate;
};

struct EntropySource {
    std::string name;
    std::string type;         // "RDRAND" "RDSEED" "TPM2" "HWRNG" "Kernel"
    bool        active;
    float       bits_per_sec;
    bool        healthy;
};

struct AuditEntry {
    long long   timestamp;
    std::string event;
    std::string actor;
    std::string result;       // "allow" "deny"
    bool        flagged;
};

class SecurityPage : public SettingsPage {
public:
    [[nodiscard]] const char* label() const override { return "Security"; }
    void load()   override;
    void render() override;

private:
    void render_enclave_tab();
    void render_entropy_tab();
    void render_audit_tab();

    EnclaveInfo              enclave_{};
    std::vector<EntropySource> entropy_sources_;
    std::vector<AuditEntry>  audit_log_;
    char                     audit_filter_[128]{};
    int                      active_tab_{0};
};

} // namespace straylight::settings
