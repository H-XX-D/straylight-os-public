// apps/settings/pages/general.h
// General system settings: hostname, timezone, locale
#pragma once

#include "../settings_page.h"

#include <string>
#include <vector>

namespace straylight::settings {

/// General settings page — hostname, timezone, and system locale.
class GeneralPage : public SettingsPage {
public:
    GeneralPage()  = default;
    ~GeneralPage() = default;

    [[nodiscard]] const char* label() const override { return "General"; }

    /// Read hostname from /etc/hostname, timezone from /etc/timezone,
    /// and locale from /etc/locale.conf or $LANG.
    void load() override;

    /// Render the general settings panel.
    void render() override;

private:
    /// Apply the hostname buffer to the running kernel and /etc/hostname.
    void apply_hostname();

    /// Apply the timezone string via timedatectl.
    void apply_timezone();

    /// Apply the selected locale to /etc/locale.conf.
    void apply_locale();

    char hostname_buf_[256] = {};
    char timezone_buf_[128] = {};

    // Available locales presented in the combo.
    static constexpr const char* kLocales[] = {
        "en_US.UTF-8",
        "en_GB.UTF-8",
        "de_DE.UTF-8",
        "fr_FR.UTF-8",
        "es_ES.UTF-8",
        "ja_JP.UTF-8",
        "zh_CN.UTF-8",
        "ko_KR.UTF-8",
        "ru_RU.UTF-8",
        "pt_BR.UTF-8",
    };
    static constexpr int kLocaleCount = 10;

    int  locale_idx_      = 0;
    bool hostname_dirty_  = false;  // User has pending unsaved change
    bool timezone_dirty_  = false;
    std::string status_msg_;        // Feedback shown after apply
};

} // namespace straylight::settings
