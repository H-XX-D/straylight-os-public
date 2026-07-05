// tools/locale/locale_manager.cpp
#include "locale_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

std::string LocaleManager::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

int LocaleManager::run_rc(const std::string& cmd) const {
    return WEXITSTATUS(std::system(cmd.c_str()));
}

LocaleManager::LocaleManager(const fs::path& config_path)
    : config_path_(config_path) {
    load();
}

LocaleStatus LocaleManager::status() const {
    LocaleStatus s = current_;

    // Refresh from system if available
    std::string lang = run_cmd("echo $LANG 2>/dev/null");
    if (!lang.empty()) s.language = lang;

    std::string tz = run_cmd("timedatectl show -p Timezone --value 2>/dev/null");
    if (tz.empty()) tz = run_cmd("cat /etc/timezone 2>/dev/null");
    if (!tz.empty()) s.timezone = tz;

    std::string kb = run_cmd("localectl status 2>/dev/null | grep 'X11 Layout' | awk '{print $3}'");
    if (!kb.empty()) s.keyboard_layout = kb;

    std::string kv = run_cmd("localectl status 2>/dev/null | grep 'X11 Variant' | awk '{print $3}'");
    if (!kv.empty()) s.keyboard_variant = kv;

    // LC_* from locale
    std::string lc_time = run_cmd("locale 2>/dev/null | grep LC_TIME | cut -d= -f2 | tr -d '\"'");
    if (!lc_time.empty()) s.date_format = lc_time;

    std::string lc_numeric = run_cmd("locale 2>/dev/null | grep LC_NUMERIC | cut -d= -f2 | tr -d '\"'");
    if (!lc_numeric.empty()) s.number_format = lc_numeric;

    std::string lc_monetary = run_cmd("locale 2>/dev/null | grep LC_MONETARY | cut -d= -f2 | tr -d '\"'");
    if (!lc_monetary.empty()) s.currency_format = lc_monetary;

    return s;
}

Result<void, std::string> LocaleManager::set_language(const std::string& lang) {
    std::string locale = lang;
    if (locale.find('.') == std::string::npos) {
        locale += ".UTF-8";
    }

    // Generate locale if needed
    run_rc("locale-gen " + locale + " 2>/dev/null");

    // Set system locale
    int rc = run_rc("localectl set-locale LANG=" + locale + " 2>&1");
    if (rc != 0) {
        // Fallback: write directly
        std::ofstream ofs("/etc/default/locale");
        if (ofs) {
            ofs << "LANG=" << locale << "\n";
        }
    }

    current_.language = locale;
    save();
    return Result<void, std::string>::ok();
}

Result<void, std::string> LocaleManager::set_timezone(const std::string& tz) {
    // Validate timezone exists
    fs::path tz_path = fs::path("/usr/share/zoneinfo") / tz;
    if (!fs::exists(tz_path)) {
        return Result<void, std::string>::error("Invalid timezone: " + tz);
    }

    int rc = run_rc("timedatectl set-timezone " + tz + " 2>&1");
    if (rc != 0) {
        // Fallback: symlink
        std::error_code ec;
        fs::remove("/etc/localtime", ec);
        fs::create_symlink(tz_path, "/etc/localtime", ec);
        if (ec) {
            return Result<void, std::string>::error("Failed to set timezone: " + ec.message());
        }
        std::ofstream ofs("/etc/timezone");
        if (ofs) ofs << tz << "\n";
    }

    current_.timezone = tz;
    save();
    return Result<void, std::string>::ok();
}

Result<void, std::string> LocaleManager::set_formats(const std::string& locale) {
    std::string loc = locale;
    if (loc.find('.') == std::string::npos) {
        loc += ".UTF-8";
    }

    // Generate locale
    run_rc("locale-gen " + loc + " 2>/dev/null");

    int rc = run_rc("localectl set-locale "
                    "LC_TIME=" + loc + " "
                    "LC_NUMERIC=" + loc + " "
                    "LC_MONETARY=" + loc + " "
                    "LC_PAPER=" + loc + " "
                    "LC_MEASUREMENT=" + loc + " 2>&1");

    if (rc != 0) {
        // Fallback: append to locale file
        std::ofstream ofs("/etc/default/locale", std::ios::app);
        if (ofs) {
            ofs << "LC_TIME=" << loc << "\n"
                << "LC_NUMERIC=" << loc << "\n"
                << "LC_MONETARY=" << loc << "\n";
        }
    }

    current_.date_format = loc;
    current_.number_format = loc;
    current_.currency_format = loc;
    save();
    return Result<void, std::string>::ok();
}

std::vector<LanguageInfo> LocaleManager::list_languages() const {
    std::vector<LanguageInfo> languages;

    // Common languages
    struct LangDef {
        const char* code;
        const char* name;
    };
    static const LangDef common[] = {
        {"en_US", "English (US)"},
        {"en_GB", "English (UK)"},
        {"de_DE", "German"},
        {"fr_FR", "French"},
        {"es_ES", "Spanish (Spain)"},
        {"es_MX", "Spanish (Mexico)"},
        {"pt_BR", "Portuguese (Brazil)"},
        {"pt_PT", "Portuguese (Portugal)"},
        {"it_IT", "Italian"},
        {"nl_NL", "Dutch"},
        {"pl_PL", "Polish"},
        {"ru_RU", "Russian"},
        {"ja_JP", "Japanese"},
        {"ko_KR", "Korean"},
        {"zh_CN", "Chinese (Simplified)"},
        {"zh_TW", "Chinese (Traditional)"},
        {"ar_SA", "Arabic"},
        {"hi_IN", "Hindi"},
        {"sv_SE", "Swedish"},
        {"nb_NO", "Norwegian"},
        {"da_DK", "Danish"},
        {"fi_FI", "Finnish"},
        {"tr_TR", "Turkish"},
        {"uk_UA", "Ukrainian"},
        {"cs_CZ", "Czech"},
        {"ro_RO", "Romanian"},
        {"hu_HU", "Hungarian"},
        {"el_GR", "Greek"},
        {"th_TH", "Thai"},
        {"vi_VN", "Vietnamese"},
        {"id_ID", "Indonesian"},
    };

    for (const auto& l : common) {
        languages.push_back({l.code, l.name, "UTF-8"});
    }

    // Also try to get system-installed locales
    std::string installed = run_cmd("locale -a 2>/dev/null");
    if (!installed.empty()) {
        std::istringstream iss(installed);
        std::string loc;
        while (std::getline(iss, loc)) {
            if (loc.find("UTF-8") != std::string::npos ||
                loc.find("utf8") != std::string::npos) {
                // Check if already in list
                std::string code = loc.substr(0, loc.find('.'));
                bool found = false;
                for (const auto& l : languages) {
                    if (l.code == code) { found = true; break; }
                }
                if (!found) {
                    languages.push_back({code, code, "UTF-8"});
                }
            }
        }
    }

    return languages;
}

std::vector<std::string> LocaleManager::list_timezones(const std::string& region) const {
    std::vector<std::string> timezones;

    std::string cmd = "timedatectl list-timezones 2>/dev/null";
    std::string output = run_cmd(cmd);

    if (output.empty()) {
        // Fallback: scan zoneinfo
        fs::path base = "/usr/share/zoneinfo";
        if (fs::exists(base)) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(base, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file()) continue;
                std::string rel = it->path().string().substr(base.string().size() + 1);
                // Skip non-timezone paths
                if (rel.find('/') == std::string::npos) continue;
                if (rel.find("posix") == 0 || rel.find("right") == 0) continue;
                timezones.push_back(rel);
            }
        }
    } else {
        std::istringstream iss(output);
        std::string tz;
        while (std::getline(iss, tz)) {
            if (!tz.empty()) timezones.push_back(tz);
        }
    }

    // Filter by region if specified
    if (!region.empty()) {
        std::vector<std::string> filtered;
        for (const auto& tz : timezones) {
            if (tz.find(region) == 0) {
                filtered.push_back(tz);
            }
        }
        return filtered;
    }

    return timezones;
}

Result<void, std::string> LocaleManager::set_keyboard(const std::string& layout,
                                                        const std::string& variant) {
    std::string cmd = "localectl set-x11-keymap " + layout;
    if (!variant.empty()) {
        cmd += " " + variant;
    }
    cmd += " 2>&1";

    int rc = run_rc(cmd);
    if (rc != 0) {
        return Result<void, std::string>::error(
            "Failed to set keyboard layout (run as root?)");
    }

    current_.keyboard_layout = layout;
    current_.keyboard_variant = variant;
    save();
    return Result<void, std::string>::ok();
}

Result<void, std::string> LocaleManager::install_dictionary(const std::string& lang) {
    // Map language code to package name
    std::string lang_short = lang.substr(0, 2);
    std::string pkg = "hunspell-" + lang_short;

    std::string cmd = "apt-get install -y " + pkg + " 2>&1";
    int rc = run_rc(cmd);
    if (rc != 0) {
        return Result<void, std::string>::error(
            "Failed to install dictionary package: " + pkg);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> LocaleManager::save() const {
    std::error_code ec;
    fs::create_directories(config_path_.parent_path(), ec);

    nlohmann::json j;
    j["language"] = current_.language;
    j["timezone"] = current_.timezone;
    j["date_format"] = current_.date_format;
    j["number_format"] = current_.number_format;
    j["currency_format"] = current_.currency_format;
    j["keyboard_layout"] = current_.keyboard_layout;
    j["keyboard_variant"] = current_.keyboard_variant;

    std::ofstream ofs(config_path_);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot write: " + config_path_.string());
    }
    ofs << j.dump(2) << "\n";
    return Result<void, std::string>::ok();
}

Result<void, std::string> LocaleManager::load() {
    if (!fs::exists(config_path_)) {
        // Initialize with system defaults
        current_ = status();
        return Result<void, std::string>::ok();
    }

    std::ifstream ifs(config_path_);
    if (!ifs) {
        return Result<void, std::string>::error("Cannot read: " + config_path_.string());
    }

    try {
        nlohmann::json j;
        ifs >> j;

        current_.language = j.value("language", "en_US.UTF-8");
        current_.timezone = j.value("timezone", "UTC");
        current_.date_format = j.value("date_format", "en_US.UTF-8");
        current_.number_format = j.value("number_format", "en_US.UTF-8");
        current_.currency_format = j.value("currency_format", "en_US.UTF-8");
        current_.keyboard_layout = j.value("keyboard_layout", "us");
        current_.keyboard_variant = j.value("keyboard_variant", "");

        return Result<void, std::string>::ok();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(
            std::string("Parse error: ") + e.what());
    }
}

} // namespace straylight
