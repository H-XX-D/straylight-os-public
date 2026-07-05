// tools/boot/boot_manager.cpp
#include "boot_manager.h"

#include <straylight/log.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Result<std::string, SLError> BootManager::exec_cmd(const std::string& cmd) const {
    std::array<char, 4096> buf;
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, SLError>::error(
            {SLErrorCode::IOError, "Failed to execute: " + cmd});
    }
    while (fgets(buf.data(), buf.size(), pipe)) {
        output += buf.data();
    }
    int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return Result<std::string, SLError>::error(
            {SLErrorCode::IOError, "Command failed: " + cmd});
    }
    return Result<std::string, SLError>::ok(std::move(output));
}

std::string BootManager::running_kernel() const {
    auto r = exec_cmd("uname -r 2>/dev/null");
    if (!r.has_value()) return "";
    std::string ver = r.value();
    while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r')) ver.pop_back();
    return ver;
}

std::string BootManager::current_cmdline() const {
    std::ifstream ifs("/proc/cmdline");
    if (!ifs) return "";
    std::string line;
    std::getline(ifs, line);
    return line;
}

// ---------------------------------------------------------------------------
// Bootloader detection
// ---------------------------------------------------------------------------

BootloaderType BootManager::detect_bootloader() const {
    // Check for systemd-boot first.
    if (fs::exists("/boot/loader/loader.conf") ||
        fs::exists("/efi/loader/loader.conf")) {
        return BootloaderType::SystemdBoot;
    }

    // Check for GRUB.
    if (fs::exists("/etc/default/grub") ||
        fs::exists("/boot/grub/grub.cfg") ||
        fs::exists("/boot/grub2/grub.cfg")) {
        return BootloaderType::Grub2;
    }

    return BootloaderType::Unknown;
}

// ---------------------------------------------------------------------------
// List kernels
// ---------------------------------------------------------------------------

Result<std::vector<KernelInfo>, SLError> BootManager::list_kernels() const {
    std::vector<KernelInfo> kernels;
    std::string running = running_kernel();

    // Scan /boot/ for vmlinuz-* files.
    if (!fs::exists("/boot")) {
        return Result<std::vector<KernelInfo>, SLError>::error(
            {SLErrorCode::NotFound, "/boot directory not found"});
    }

    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/boot", ec)) {
        std::string name = entry.path().filename().string();
        if (name.substr(0, 8) != "vmlinuz-") continue;

        KernelInfo ki;
        ki.version = name.substr(8); // Strip "vmlinuz-"
        ki.path = entry.path().string();
        ki.is_running = (ki.version == running);

        // Look for matching initrd.
        for (const auto& initrd_prefix : {"initrd.img-", "initramfs-"}) {
            auto initrd_path = fs::path("/boot") / (std::string(initrd_prefix) + ki.version);
            if (fs::exists(initrd_path)) {
                ki.initrd_path = initrd_path.string();
                break;
            }
            // Also check with .img suffix.
            auto initrd_img = fs::path("/boot") / (std::string(initrd_prefix) + ki.version + ".img");
            if (fs::exists(initrd_img)) {
                ki.initrd_path = initrd_img.string();
                break;
            }
        }

        // Config file.
        auto config_path = fs::path("/boot") / ("config-" + ki.version);
        if (fs::exists(config_path)) {
            ki.config_path = config_path.string();
        }

        kernels.push_back(ki);
    }

    // Sort by version (newest first based on lexicographic comparison).
    std::sort(kernels.begin(), kernels.end(),
              [](const KernelInfo& a, const KernelInfo& b) {
                  return a.version > b.version;
              });

    // Determine default.
    auto bl = detect_bootloader();
    if (bl == BootloaderType::Grub2) {
        auto grub = parse_grub_config();
        if (grub.has_value() && !grub.value().default_entry.empty()) {
            // If default is "0", mark the first kernel.
            if (grub.value().default_entry == "0" && !kernels.empty()) {
                kernels[0].is_default = true;
            } else {
                // Try to match by version string.
                for (auto& k : kernels) {
                    if (grub.value().default_entry.find(k.version) != std::string::npos) {
                        k.is_default = true;
                        break;
                    }
                }
            }
        } else if (!kernels.empty()) {
            kernels[0].is_default = true;
        }
    } else if (bl == BootloaderType::SystemdBoot) {
        auto entries = list_sd_boot_entries();
        if (entries.has_value()) {
            for (const auto& entry : entries.value()) {
                if (entry.is_default) {
                    for (auto& k : kernels) {
                        if (entry.linux_path.find(k.version) != std::string::npos) {
                            k.is_default = true;
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }

    return Result<std::vector<KernelInfo>, SLError>::ok(std::move(kernels));
}

// ---------------------------------------------------------------------------
// Set default
// ---------------------------------------------------------------------------

Result<void, SLError> BootManager::set_default(const std::string& kernel_version) const {
    auto bl = detect_bootloader();

    if (bl == BootloaderType::Grub2) {
        // Find the GRUB menu entry index for this kernel.
        // Simpler approach: set GRUB_DEFAULT to the saved entry name.
        auto grub = parse_grub_config();
        if (!grub.has_value()) return Result<void, SLError>::error(grub.error());

        GrubConfig cfg = grub.value();

        // Set GRUB_DEFAULT to the kernel version string or "saved".
        // For Debian-style GRUB, use grub-set-default.
        std::string cmd = "grub-set-default 'Advanced options for StrayLight OS>"
                          "StrayLight OS, with Linux " + kernel_version + "' 2>&1";
        auto r = exec_cmd(cmd);

        // Also set GRUB_DEFAULT=saved in /etc/default/grub.
        cfg.default_entry = "saved";
        return write_grub_config(cfg);

    } else if (bl == BootloaderType::SystemdBoot) {
        // Find the entry for this kernel.
        auto entries = list_sd_boot_entries();
        if (!entries.has_value()) return Result<void, SLError>::error(entries.error());

        for (const auto& entry : entries.value()) {
            if (entry.linux_path.find(kernel_version) != std::string::npos) {
                // Write default to loader.conf.
                std::string loader_conf;
                for (const auto& path : {"/boot/loader/loader.conf", "/efi/loader/loader.conf"}) {
                    if (fs::exists(path)) { loader_conf = path; break; }
                }
                if (loader_conf.empty()) {
                    return Result<void, SLError>::error(
                        {SLErrorCode::NotFound, "loader.conf not found"});
                }

                std::ifstream ifs(loader_conf);
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                ifs.close();

                // Replace or add default line.
                auto pos = content.find("default ");
                if (pos != std::string::npos) {
                    auto end = content.find('\n', pos);
                    content.replace(pos, end - pos, "default " + entry.id + ".conf");
                } else {
                    content += "default " + entry.id + ".conf\n";
                }

                std::ofstream ofs(loader_conf, std::ios::trunc);
                if (!ofs) {
                    return Result<void, SLError>::error(
                        {SLErrorCode::PermissionDenied, "Cannot write " + loader_conf});
                }
                ofs << content;

                SL_INFO("boot: set default to {} ({})", entry.id, kernel_version);
                return Result<void, SLError>::ok();
            }
        }

        return Result<void, SLError>::error(
            {SLErrorCode::NotFound,
             "No systemd-boot entry found for kernel " + kernel_version});
    }

    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Cannot detect bootloader"});
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

Result<void, SLError> BootManager::add_param(const std::string& param) const {
    auto bl = detect_bootloader();
    if (bl == BootloaderType::Grub2) return modify_grub_cmdline(param, true);
    if (bl == BootloaderType::SystemdBoot) return modify_sd_boot_options(param, true);
    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Cannot detect bootloader"});
}

Result<void, SLError> BootManager::remove_param(const std::string& param) const {
    auto bl = detect_bootloader();
    if (bl == BootloaderType::Grub2) return modify_grub_cmdline(param, false);
    if (bl == BootloaderType::SystemdBoot) return modify_sd_boot_options(param, false);
    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Cannot detect bootloader"});
}

Result<void, SLError> BootManager::modify_grub_cmdline(const std::string& param,
                                                        bool add) const {
    auto grub = parse_grub_config();
    if (!grub.has_value()) return Result<void, SLError>::error(grub.error());

    GrubConfig cfg = grub.value();
    std::string& cmdline = cfg.cmdline_default;

    if (add) {
        // Check if already present.
        if (cmdline.find(param) != std::string::npos) {
            return Result<void, SLError>::ok(); // Already present.
        }
        if (!cmdline.empty()) cmdline += " ";
        cmdline += param;
    } else {
        // Remove the parameter.
        // Handle both "param" and "param=value" forms.
        std::string pattern = "(^|\\s)" + param + "(\\s|$)";
        // Use simple string replacement for exact match.
        auto pos = cmdline.find(param);
        if (pos == std::string::npos) {
            return Result<void, SLError>::error(
                {SLErrorCode::NotFound, "Parameter not found: " + param});
        }

        // Find the extent of this parameter (until next space or end).
        size_t end = pos;
        while (end < cmdline.size() && cmdline[end] != ' ') ++end;

        // Remove the parameter and any trailing space.
        if (end < cmdline.size()) ++end; // Skip trailing space.
        cmdline.erase(pos, end - pos);

        // Trim trailing whitespace.
        while (!cmdline.empty() && cmdline.back() == ' ') cmdline.pop_back();
    }

    return write_grub_config(cfg);
}

Result<void, SLError> BootManager::modify_sd_boot_options(const std::string& param,
                                                           bool add) const {
    auto entries = list_sd_boot_entries();
    if (!entries.has_value()) return Result<void, SLError>::error(entries.error());

    // Modify the default entry.
    for (const auto& entry : entries.value()) {
        if (!entry.is_default) continue;

        // Find the entry file.
        std::string entry_path;
        for (const auto& base : {"/boot/loader/entries/", "/efi/loader/entries/"}) {
            std::string path = std::string(base) + entry.id + ".conf";
            if (fs::exists(path)) { entry_path = path; break; }
        }

        if (entry_path.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::NotFound, "Entry file not found for: " + entry.id});
        }

        std::ifstream ifs(entry_path);
        if (!ifs) {
            return Result<void, SLError>::error(
                {SLErrorCode::IOError, "Cannot read " + entry_path});
        }

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        // Find the options line.
        auto opts_pos = content.find("options ");
        if (opts_pos == std::string::npos) {
            if (add) {
                content += "options " + param + "\n";
            }
        } else {
            auto line_end = content.find('\n', opts_pos);
            std::string options_line = content.substr(opts_pos + 8,
                                                      line_end - opts_pos - 8);

            if (add) {
                if (options_line.find(param) == std::string::npos) {
                    options_line += " " + param;
                }
            } else {
                auto ppos = options_line.find(param);
                if (ppos == std::string::npos) {
                    return Result<void, SLError>::error(
                        {SLErrorCode::NotFound, "Parameter not found: " + param});
                }
                size_t pend = ppos;
                while (pend < options_line.size() && options_line[pend] != ' ') ++pend;
                if (pend < options_line.size()) ++pend;
                options_line.erase(ppos, pend - ppos);
                while (!options_line.empty() && options_line.back() == ' ')
                    options_line.pop_back();
            }

            content.replace(opts_pos, line_end - opts_pos,
                           "options " + options_line);
        }

        std::ofstream ofs(entry_path, std::ios::trunc);
        if (!ofs) {
            return Result<void, SLError>::error(
                {SLErrorCode::PermissionDenied, "Cannot write " + entry_path});
        }
        ofs << content;

        return Result<void, SLError>::ok();
    }

    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "No default boot entry found"});
}

// ---------------------------------------------------------------------------
// Timeout
// ---------------------------------------------------------------------------

Result<void, SLError> BootManager::set_timeout(int seconds) const {
    auto bl = detect_bootloader();

    if (bl == BootloaderType::Grub2) {
        auto grub = parse_grub_config();
        if (!grub.has_value()) return Result<void, SLError>::error(grub.error());
        GrubConfig cfg = grub.value();
        cfg.timeout = seconds;
        return write_grub_config(cfg);

    } else if (bl == BootloaderType::SystemdBoot) {
        std::string loader_conf;
        for (const auto& path : {"/boot/loader/loader.conf", "/efi/loader/loader.conf"}) {
            if (fs::exists(path)) { loader_conf = path; break; }
        }
        if (loader_conf.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::NotFound, "loader.conf not found"});
        }

        std::ifstream ifs(loader_conf);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        auto pos = content.find("timeout ");
        std::string timeout_line = "timeout " + std::to_string(seconds);
        if (pos != std::string::npos) {
            auto end = content.find('\n', pos);
            content.replace(pos, end - pos, timeout_line);
        } else {
            content += timeout_line + "\n";
        }

        std::ofstream ofs(loader_conf, std::ios::trunc);
        if (!ofs) {
            return Result<void, SLError>::error(
                {SLErrorCode::PermissionDenied, "Cannot write " + loader_conf});
        }
        ofs << content;
        return Result<void, SLError>::ok();
    }

    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Cannot detect bootloader"});
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

Result<BootStatus, SLError> BootManager::get_status() const {
    BootStatus status;
    status.current_kernel = running_kernel();
    status.cmdline = current_cmdline();
    status.bootloader = detect_bootloader();

    // Root device from cmdline.
    auto cmdline = status.cmdline;
    auto root_pos = cmdline.find("root=");
    if (root_pos != std::string::npos) {
        auto end = cmdline.find(' ', root_pos);
        status.root_device = cmdline.substr(root_pos + 5,
                                            (end == std::string::npos ? cmdline.size() : end) - root_pos - 5);
    }

    // Boot mode.
    if (fs::exists("/sys/firmware/efi")) {
        status.boot_mode = "UEFI";
        // Check secure boot.
        std::ifstream sb("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
                         std::ios::binary);
        if (sb) {
            // The last byte indicates secure boot status.
            sb.seekg(-1, std::ios::end);
            char byte = 0;
            sb.read(&byte, 1);
            status.secure_boot = (byte != 0);
        }
    } else {
        status.boot_mode = "BIOS/Legacy";
    }

    // Bootloader version.
    if (status.bootloader == BootloaderType::Grub2) {
        auto r = exec_cmd("grub-install --version 2>/dev/null || grub2-install --version 2>/dev/null");
        if (r.has_value()) {
            status.bootloader_version = r.value();
            while (!status.bootloader_version.empty() &&
                   (status.bootloader_version.back() == '\n' ||
                    status.bootloader_version.back() == '\r')) {
                status.bootloader_version.pop_back();
            }
        }
    } else if (status.bootloader == BootloaderType::SystemdBoot) {
        auto r = exec_cmd("bootctl --version 2>/dev/null");
        if (r.has_value()) {
            status.bootloader_version = r.value();
            auto nl = status.bootloader_version.find('\n');
            if (nl != std::string::npos) status.bootloader_version.resize(nl);
        }
    }

    // Timeout.
    if (status.bootloader == BootloaderType::Grub2) {
        auto grub = parse_grub_config();
        if (grub.has_value()) status.default_timeout = grub.value().timeout;
    }

    return Result<BootStatus, SLError>::ok(std::move(status));
}

// ---------------------------------------------------------------------------
// Show config
// ---------------------------------------------------------------------------

Result<std::string, SLError> BootManager::show_config() const {
    auto bl = detect_bootloader();

    if (bl == BootloaderType::Grub2) {
        std::ifstream ifs("/etc/default/grub");
        if (!ifs) {
            return Result<std::string, SLError>::error(
                {SLErrorCode::NotFound, "/etc/default/grub not found"});
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        return Result<std::string, SLError>::ok("=== /etc/default/grub ===\n\n" + content);

    } else if (bl == BootloaderType::SystemdBoot) {
        std::string output;
        std::string loader_conf;
        for (const auto& path : {"/boot/loader/loader.conf", "/efi/loader/loader.conf"}) {
            if (fs::exists(path)) { loader_conf = path; break; }
        }

        if (!loader_conf.empty()) {
            std::ifstream ifs(loader_conf);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
            output += "=== " + loader_conf + " ===\n\n" + content + "\n";
        }

        // Show entries.
        auto entries = list_sd_boot_entries();
        if (entries.has_value()) {
            output += "=== Boot Entries ===\n\n";
            for (const auto& e : entries.value()) {
                output += "[" + e.id + "]";
                if (e.is_default) output += " (default)";
                output += "\n";
                output += "  title:   " + e.title + "\n";
                output += "  linux:   " + e.linux_path + "\n";
                output += "  initrd:  " + e.initrd_path + "\n";
                output += "  options: " + e.options + "\n\n";
            }
        }

        return Result<std::string, SLError>::ok(std::move(output));
    }

    return Result<std::string, SLError>::error(
        {SLErrorCode::NotFound, "Cannot detect bootloader"});
}

// ---------------------------------------------------------------------------
// Rebuild initramfs
// ---------------------------------------------------------------------------

Result<void, SLError> BootManager::rebuild_initramfs(const std::string& version) const {
    std::string target = version.empty() ? running_kernel() : version;

    // Try update-initramfs (Debian/Ubuntu).
    std::string cmd = "update-initramfs -u -k " + target + " 2>&1";
    auto r = exec_cmd(cmd);
    if (r.has_value()) {
        SL_INFO("boot: rebuilt initramfs for {}", target);
        return Result<void, SLError>::ok();
    }

    // Try dracut (Fedora/RHEL).
    cmd = "dracut --force --kver " + target + " 2>&1";
    r = exec_cmd(cmd);
    if (r.has_value()) {
        SL_INFO("boot: rebuilt initramfs via dracut for {}", target);
        return Result<void, SLError>::ok();
    }

    // Try mkinitcpio (Arch).
    cmd = "mkinitcpio -p linux 2>&1";
    r = exec_cmd(cmd);
    if (r.has_value()) {
        SL_INFO("boot: rebuilt initramfs via mkinitcpio");
        return Result<void, SLError>::ok();
    }

    return Result<void, SLError>::error(
        {SLErrorCode::NotFound,
         "No initramfs tool found (tried update-initramfs, dracut, mkinitcpio)"});
}

// ---------------------------------------------------------------------------
// GRUB config
// ---------------------------------------------------------------------------

Result<GrubConfig, SLError> BootManager::parse_grub_config() const {
    std::ifstream ifs("/etc/default/grub");
    if (!ifs) {
        return Result<GrubConfig, SLError>::error(
            {SLErrorCode::NotFound, "/etc/default/grub not found"});
    }

    GrubConfig cfg;
    std::string line;

    while (std::getline(ifs, line)) {
        // Skip comments and empty lines.
        if (line.empty() || line[0] == '#') continue;

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);

        // Strip quotes.
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (key == "GRUB_CMDLINE_LINUX_DEFAULT") {
            cfg.cmdline_default = val;
        } else if (key == "GRUB_CMDLINE_LINUX") {
            cfg.cmdline_linux = val;
        } else if (key == "GRUB_TIMEOUT") {
            try { cfg.timeout = std::stoi(val); } catch (...) {}
        } else if (key == "GRUB_DEFAULT") {
            cfg.default_entry = val;
        } else if (key == "GRUB_DISTRIBUTOR") {
            cfg.distributor = val;
        }
    }

    return Result<GrubConfig, SLError>::ok(std::move(cfg));
}

Result<void, SLError> BootManager::write_grub_config(const GrubConfig& config) const {
    // Read the existing file to preserve comments and ordering.
    std::ifstream ifs("/etc/default/grub");
    if (!ifs) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "/etc/default/grub not found"});
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Update each value.
    auto update_value = [&](const std::string& key, const std::string& value) {
        std::string search = key + "=";
        auto pos = content.find(search);
        if (pos != std::string::npos) {
            // Skip if this line is commented out — find the actual line start.
            auto line_start = content.rfind('\n', pos);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            if (content[line_start] == '#') return; // Commented out, skip.

            auto end = content.find('\n', pos);
            content.replace(pos, end - pos, key + "=\"" + value + "\"");
        }
    };

    update_value("GRUB_CMDLINE_LINUX_DEFAULT", config.cmdline_default);
    update_value("GRUB_TIMEOUT", std::to_string(config.timeout));
    if (!config.default_entry.empty()) {
        update_value("GRUB_DEFAULT", config.default_entry);
    }

    std::ofstream ofs("/etc/default/grub", std::ios::trunc);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::PermissionDenied, "Cannot write /etc/default/grub"});
    }
    ofs << content;

    // Run update-grub.
    return update_bootloader();
}

Result<void, SLError> BootManager::update_bootloader() const {
    auto bl = detect_bootloader();
    std::string cmd;

    if (bl == BootloaderType::Grub2) {
        // Try update-grub (Debian), grub2-mkconfig (RHEL), grub-mkconfig (Arch).
        cmd = "update-grub 2>&1";
        auto r = exec_cmd(cmd);
        if (!r.has_value()) {
            cmd = "grub2-mkconfig -o /boot/grub2/grub.cfg 2>&1";
            r = exec_cmd(cmd);
        }
        if (!r.has_value()) {
            cmd = "grub-mkconfig -o /boot/grub/grub.cfg 2>&1";
            r = exec_cmd(cmd);
        }
        if (!r.has_value()) {
            return Result<void, SLError>::error(
                {SLErrorCode::IOError, "Failed to update GRUB configuration"});
        }
    } else if (bl == BootloaderType::SystemdBoot) {
        cmd = "bootctl update 2>&1";
        auto r = exec_cmd(cmd);
        if (!r.has_value()) {
            SL_WARN("boot: bootctl update returned error (may be normal)");
        }
    }

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// systemd-boot entries
// ---------------------------------------------------------------------------

Result<std::vector<SystemdBootEntry>, SLError> BootManager::list_sd_boot_entries() const {
    std::vector<SystemdBootEntry> entries;

    // Find the entries directory.
    std::string entries_dir;
    for (const auto& base : {"/boot/loader/entries", "/efi/loader/entries"}) {
        if (fs::exists(base)) { entries_dir = base; break; }
    }

    if (entries_dir.empty()) {
        return Result<std::vector<SystemdBootEntry>, SLError>::error(
            {SLErrorCode::NotFound, "systemd-boot entries directory not found"});
    }

    // Find the default entry.
    std::string default_id;
    for (const auto& path : {"/boot/loader/loader.conf", "/efi/loader/loader.conf"}) {
        std::ifstream ifs(path);
        if (!ifs) continue;
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find("default ") == 0) {
                default_id = line.substr(8);
                // Strip .conf suffix if present.
                if (default_id.size() > 5 &&
                    default_id.substr(default_id.size() - 5) == ".conf") {
                    default_id.resize(default_id.size() - 5);
                }
                // Handle wildcards (e.g., "straylight-*").
                break;
            }
        }
    }

    std::error_code ec;
    for (auto& file : fs::directory_iterator(entries_dir, ec)) {
        if (file.path().extension() != ".conf") continue;

        auto entry_result = parse_sd_boot_entry(file.path().string());
        if (!entry_result.has_value()) continue;

        auto entry = entry_result.value();
        entry.id = file.path().stem().string();

        // Check if this is the default.
        if (!default_id.empty()) {
            if (default_id.back() == '*') {
                // Wildcard match.
                std::string prefix = default_id.substr(0, default_id.size() - 1);
                entry.is_default = (entry.id.substr(0, prefix.size()) == prefix);
            } else {
                entry.is_default = (entry.id == default_id);
            }
        }

        entries.push_back(std::move(entry));
    }

    // Sort by ID.
    std::sort(entries.begin(), entries.end(),
              [](const SystemdBootEntry& a, const SystemdBootEntry& b) {
                  return a.id > b.id; // Newest first.
              });

    return Result<std::vector<SystemdBootEntry>, SLError>::ok(std::move(entries));
}

Result<SystemdBootEntry, SLError> BootManager::parse_sd_boot_entry(
    const std::string& path) const {
    std::ifstream ifs(path);
    if (!ifs) {
        return Result<SystemdBootEntry, SLError>::error(
            {SLErrorCode::IOError, "Cannot read " + path});
    }

    SystemdBootEntry entry;
    std::string line;

    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto space = line.find(' ');
        if (space == std::string::npos) continue;

        std::string key = line.substr(0, space);
        std::string val = line.substr(space + 1);
        // Trim.
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);

        if (key == "title") entry.title = val;
        else if (key == "linux") entry.linux_path = val;
        else if (key == "initrd") entry.initrd_path = val;
        else if (key == "options") entry.options = val;
    }

    return Result<SystemdBootEntry, SLError>::ok(std::move(entry));
}

} // namespace straylight
