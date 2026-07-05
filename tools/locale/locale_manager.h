// tools/locale/locale_manager.h
// Locale and language manager — timezone, language, formats, keyboard layout.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Current locale/language status.
struct LocaleStatus {
    std::string language;       // e.g., "en_US.UTF-8"
    std::string timezone;       // e.g., "America/New_York"
    std::string date_format;    // e.g., "en_US.UTF-8"
    std::string number_format;  // e.g., "en_US.UTF-8"
    std::string currency_format;// e.g., "en_US.UTF-8"
    std::string keyboard_layout;// e.g., "us"
    std::string keyboard_variant;
};

/// Language info.
struct LanguageInfo {
    std::string code;        // e.g., "en_US"
    std::string name;        // e.g., "English (US)"
    std::string charset;     // e.g., "UTF-8"
};

class LocaleManager {
public:
    explicit LocaleManager(const std::filesystem::path& config_path =
                            "/etc/straylight/locale.conf");

    /// Get current locale status.
    LocaleStatus status() const;

    /// Set the system language/locale.
    Result<void, std::string> set_language(const std::string& lang);

    /// Set the system timezone.
    Result<void, std::string> set_timezone(const std::string& tz);

    /// Set format preferences (date, number, currency).
    Result<void, std::string> set_formats(const std::string& locale);

    /// List available languages.
    std::vector<LanguageInfo> list_languages() const;

    /// List available timezones (optionally filtered by region).
    std::vector<std::string> list_timezones(const std::string& region = "") const;

    /// Set keyboard layout (coordinates with input tool).
    Result<void, std::string> set_keyboard(const std::string& layout,
                                            const std::string& variant = "");

    /// Install spell-check dictionary for a language.
    Result<void, std::string> install_dictionary(const std::string& lang);

    /// Save preferences to StrayLight config.
    Result<void, std::string> save() const;

    /// Load preferences from StrayLight config.
    Result<void, std::string> load();

private:
    /// Run a command and return output.
    std::string run_cmd(const std::string& cmd) const;

    /// Run a command and return exit code.
    int run_rc(const std::string& cmd) const;

    std::filesystem::path config_path_;
    LocaleStatus current_;
};

} // namespace straylight
