// apps/settings/pages/general.cpp
// General settings: hostname, timezone, locale
#include "general.h"

#include <imgui.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace straylight::settings {

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

void GeneralPage::load() {
    // Hostname from /etc/hostname (prefer file over gethostname to avoid
    // transient in-kernel truncation)
    {
        std::ifstream f("/etc/hostname");
        if (f.is_open()) {
            std::string line;
            if (std::getline(f, line)) {
                strncpy(hostname_buf_, line.c_str(), sizeof(hostname_buf_) - 1);
                hostname_buf_[sizeof(hostname_buf_) - 1] = '\0';
            }
        } else {
            // Fallback: kernel hostname
            gethostname(hostname_buf_, sizeof(hostname_buf_) - 1);
        }
    }

    // Timezone from /etc/timezone (plain text file on Debian/Ubuntu/Arch)
    {
        std::ifstream f("/etc/timezone");
        if (f.is_open()) {
            std::string line;
            if (std::getline(f, line)) {
                strncpy(timezone_buf_, line.c_str(), sizeof(timezone_buf_) - 1);
                timezone_buf_[sizeof(timezone_buf_) - 1] = '\0';
            }
        }
    }

    // Detect current locale from /etc/locale.conf (LANG=... lines)
    {
        std::ifstream f("/etc/locale.conf");
        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 5, "LANG=") == 0) {
                std::string lang = line.substr(5);
                for (int i = 0; i < kLocaleCount; ++i) {
                    if (lang == kLocales[i]) {
                        locale_idx_ = i;
                        break;
                    }
                }
                break;
            }
        }
    }
    // Fallback: check $LANG environment variable
    if (locale_idx_ == 0) {
        const char* env = getenv("LANG");
        if (env) {
            for (int i = 0; i < kLocaleCount; ++i) {
                if (strcmp(env, kLocales[i]) == 0) {
                    locale_idx_ = i;
                    break;
                }
            }
        }
    }

    hostname_dirty_ = false;
    timezone_dirty_ = false;
    status_msg_.clear();
}

// ---------------------------------------------------------------------------
// apply helpers
// ---------------------------------------------------------------------------

void GeneralPage::apply_hostname() {
    // Update kernel hostname
    if (sethostname(hostname_buf_, strlen(hostname_buf_)) != 0) {
        status_msg_ = std::string("sethostname failed: ") + strerror(errno);
        return;
    }

    // Persist to /etc/hostname
    {
        std::string tmp = "/etc/hostname.tmp";
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) {
            status_msg_ = "Cannot write /etc/hostname";
            return;
        }
        f << hostname_buf_ << "\n";
        f.close();
        std::rename(tmp.c_str(), "/etc/hostname");
    }

    hostname_dirty_ = false;
    status_msg_ = std::string("Hostname set to: ") + hostname_buf_;
}

void GeneralPage::apply_timezone() {
    // Use timedatectl if available, otherwise write symlink manually
    std::string cmd = "timedatectl set-timezone '";
    cmd += timezone_buf_;
    cmd += "' 2>/dev/null";
    int rc = std::system(cmd.c_str());

    if (rc != 0) {
        // Fallback: manually update /etc/localtime symlink
        std::string zi = "/usr/share/zoneinfo/";
        zi += timezone_buf_;
        std::string link_cmd = "ln -sf '" + zi + "' /etc/localtime 2>/dev/null";
        rc = std::system(link_cmd.c_str());

        // Also write /etc/timezone
        std::ofstream f("/etc/timezone", std::ios::trunc);
        if (f.is_open()) {
            f << timezone_buf_ << "\n";
        }
    }

    timezone_dirty_ = false;
    status_msg_     = rc == 0
                          ? std::string("Timezone set to: ") + timezone_buf_
                          : "Failed to set timezone (check /usr/share/zoneinfo/)";
}

void GeneralPage::apply_locale() {
    std::ofstream f("/etc/locale.conf", std::ios::trunc);
    if (!f.is_open()) {
        status_msg_ = "Cannot write /etc/locale.conf";
        return;
    }
    f << "LANG=" << kLocales[locale_idx_] << "\n";
    f << "LC_ALL=" << kLocales[locale_idx_] << "\n";
    f.close();

    // Also generate the locale on Debian-based systems
    std::string gen_cmd = "locale-gen '";
    gen_cmd += kLocales[locale_idx_];
    gen_cmd += "' 2>/dev/null";
    const int gen_rc = std::system(gen_cmd.c_str());

    status_msg_ = std::string("Locale set to: ") + kLocales[locale_idx_] +
                  " (re-login to apply)" +
                  (gen_rc == 0 ? "" : " (locale-gen failed)");
}

// ---------------------------------------------------------------------------
// render()
// ---------------------------------------------------------------------------

void GeneralPage::render() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("General");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // --- Hostname -------------------------------------------------------
    ImGui::SeparatorText("Hostname");
    ImGui::Text("The machine name visible on the network.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::InputText("##hostname", hostname_buf_, sizeof(hostname_buf_))) {
        hostname_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply##host")) {
        apply_hostname();
    }
    if (hostname_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(unsaved)");
    }

    ImGui::Spacing();

    // --- Timezone -------------------------------------------------------
    ImGui::SeparatorText("Timezone");
    ImGui::TextDisabled("Format: Region/City  (e.g. America/New_York, Europe/Berlin)");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::InputText("##tz", timezone_buf_, sizeof(timezone_buf_))) {
        timezone_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply##tz")) {
        apply_timezone();
    }
    if (timezone_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(unsaved)");
    }

    ImGui::Spacing();

    // --- Locale ---------------------------------------------------------
    ImGui::SeparatorText("System Language / Locale");
    ImGui::TextDisabled("Applies LANG and LC_ALL to /etc/locale.conf.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(200.0f);
    int prev_idx = locale_idx_;
    if (ImGui::Combo("Locale##lang", &locale_idx_, kLocales, kLocaleCount)) {
        if (locale_idx_ != prev_idx) {
            apply_locale();
        }
    }

    // --- Status bar -----------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    if (!status_msg_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f),
                           "%s", status_msg_.c_str());
    }
}

} // namespace straylight::settings
