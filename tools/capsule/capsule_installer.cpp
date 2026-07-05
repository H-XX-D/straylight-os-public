// tools/capsule/capsule_installer.cpp
#include "capsule_installer.h"

#include <straylight/ipc_client.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <sys/sysinfo.h>
#include <unistd.h>

namespace straylight {

uint32_t CapsuleInstaller::get_system_ram_mb() {
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        return static_cast<uint32_t>(si.totalram * si.mem_unit / (1024 * 1024));
    }
    // Fallback: parse /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            uint64_t kb = 0;
            std::sscanf(line.c_str(), "MemTotal: %lu kB", &kb);
            return static_cast<uint32_t>(kb / 1024);
        }
    }
    return 0;
}

uint32_t CapsuleInstaller::get_system_vram_mb() {
    // Query VPU sysfs — StrayLight's custom VPU driver exposes VRAM size
    std::ifstream vram_file("/sys/class/vpu/vpu0/vram_total_mb");
    if (vram_file.is_open()) {
        uint32_t mb = 0;
        vram_file >> mb;
        return mb;
    }

    // Fallback: check DRM subsystem for VRAM
    for (int i = 0; i < 4; ++i) {
        std::string path = "/sys/class/drm/card" + std::to_string(i) +
                           "/device/mem_info_vram_total";
        std::ifstream drm_file(path);
        if (drm_file.is_open()) {
            uint64_t bytes = 0;
            drm_file >> bytes;
            return static_cast<uint32_t>(bytes / (1024 * 1024));
        }
    }

    return 0;
}

uint32_t CapsuleInstaller::get_cpu_core_count() {
    long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 0 ? static_cast<uint32_t>(cores) : 1;
}

bool CapsuleInstaller::is_mesh_available() {
    // Check if mesh daemon socket exists and is connectable
    IpcJsonClient client;
    auto result = client.connect("/run/straylight/mesh.sock");
    if (result.has_value()) {
        // Send a ping to verify the service is responsive
        nlohmann::json ping;
        ping["jsonrpc"] = "2.0";
        ping["method"] = "ping";
        ping["id"] = 1;
        auto resp = client.request(ping);
        client.disconnect();
        return resp.has_value();
    }
    return false;
}

Result<void, std::string> CapsuleInstaller::verify_hash(
    const std::filesystem::path& archive_path) {
    auto sidecar = archive_path.string() + ".sha256";
    if (!std::filesystem::exists(sidecar)) {
        // No sidecar — skip verification but warn
        std::cerr << "capsule: warning: no .sha256 sidecar found, skipping hash verification\n";
        return Result<void, std::string>::ok();
    }

    // Read expected hash from sidecar
    std::ifstream ifs(sidecar);
    std::string expected_hash;
    ifs >> expected_hash;

    if (expected_hash.size() != 64) {
        return Result<void, std::string>::error(
            "Invalid hash in sidecar file: " + sidecar);
    }

    // Compute actual hash
    std::string cmd = "sha256sum '" + archive_path.string() + "' 2>/dev/null || " +
                      "shasum -a 256 '" + archive_path.string() + "' 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to run sha256sum");
    }

    std::string output;
    std::array<char, 256> buf;
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    ::pclose(pipe);

    std::string actual_hash;
    for (char c : output) {
        if (std::isxdigit(c) && actual_hash.size() < 64) {
            actual_hash += c;
        } else if (actual_hash.size() == 64) {
            break;
        }
    }

    if (actual_hash != expected_hash) {
        return Result<void, std::string>::error(
            "Hash mismatch — archive may be corrupted or tampered with.\n"
            "  expected: " + expected_hash + "\n"
            "  actual:   " + actual_hash);
    }

    return Result<void, std::string>::ok();
}

Result<std::filesystem::path, std::string> CapsuleInstaller::extract_archive(
    const std::filesystem::path& archive_path) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "capsule-extract";
    std::filesystem::create_directories(tmp_dir);

    // Extract tar.zst
    std::string cmd = "tar --zstd -xf '" + archive_path.string() +
                      "' -C '" + tmp_dir.string() + "' 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::filesystem::path, std::string>::error(
            "Failed to execute tar command");
    }

    std::string output;
    std::array<char, 256> buf;
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    int status = ::pclose(pipe);

    if (status != 0) {
        // Fallback: pipe through zstd
        std::string fallback = "zstd -d -c '" + archive_path.string() +
                               "' | tar -xf - -C '" + tmp_dir.string() + "' 2>&1";
        pipe = ::popen(fallback.c_str(), "r");
        if (!pipe) {
            return Result<std::filesystem::path, std::string>::error(
                "Failed to execute zstd|tar command");
        }
        output.clear();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            output += buf.data();
        }
        status = ::pclose(pipe);
        if (status != 0) {
            return Result<std::filesystem::path, std::string>::error(
                "Archive extraction failed: " + output);
        }
    }

    // Find the extracted directory (should be the only entry)
    for (const auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
        if (entry.is_directory()) {
            return Result<std::filesystem::path, std::string>::ok(entry.path());
        }
    }

    return Result<std::filesystem::path, std::string>::error(
        "No directory found after extraction in " + tmp_dir.string());
}

Result<void, std::string> CapsuleInstaller::check_system_capabilities(
    const ResourceContract& contract) {
    std::vector<std::string> failures;

    if (contract.min_ram_mb > 0) {
        uint32_t system_ram = get_system_ram_mb();
        if (system_ram > 0 && system_ram < contract.min_ram_mb) {
            failures.push_back("RAM: need " + std::to_string(contract.min_ram_mb) +
                             " MB, have " + std::to_string(system_ram) + " MB");
        }
    }

    if (contract.min_vram_mb > 0) {
        uint32_t system_vram = get_system_vram_mb();
        if (system_vram > 0 && system_vram < contract.min_vram_mb) {
            failures.push_back("VRAM: need " + std::to_string(contract.min_vram_mb) +
                             " MB, have " + std::to_string(system_vram) + " MB");
        }
    }

    if (contract.min_cpu_cores > 0) {
        uint32_t cores = get_cpu_core_count();
        if (cores < contract.min_cpu_cores) {
            failures.push_back("CPU: need " + std::to_string(contract.min_cpu_cores) +
                             " cores, have " + std::to_string(cores));
        }
    }

    if (contract.requires_mesh) {
        if (!is_mesh_available()) {
            failures.push_back("Mesh: service not available");
        }
    }

    if (!failures.empty()) {
        std::string msg = "System does not meet resource contract:\n";
        for (const auto& f : failures) {
            msg += "  - " + f + "\n";
        }
        return Result<void, std::string>::error(msg);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleInstaller::install_dependencies(
    const std::vector<std::string>& dependencies) {
    if (dependencies.empty()) {
        return Result<void, std::string>::ok();
    }

    std::string pkg_list;
    for (const auto& dep : dependencies) {
        // Sanitize package names
        bool valid = true;
        for (char c : dep) {
            if (!std::isalnum(c) && c != '-' && c != '.' && c != '+' && c != ':') {
                valid = false;
                break;
            }
        }
        if (!valid) {
            return Result<void, std::string>::error(
                "Invalid dependency name: " + dep);
        }
        pkg_list += " " + dep;
    }

    std::cout << "capsule: installing dependencies:" << pkg_list << "\n";

    std::string cmd = "apt install -y" + pkg_list + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to run apt install");
    }

    std::string output;
    std::array<char, 512> buf;
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    int status = ::pclose(pipe);

    if (status != 0) {
        return Result<void, std::string>::error(
            "apt install failed:\n" + output);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleInstaller::register_with_quota(
    const CapsuleManifest& manifest) {
    IpcJsonClient client;
    auto conn = client.connect("/run/straylight/quota.sock");
    if (!conn.has_value()) {
        std::cerr << "capsule: warning: quota service unavailable, skipping registration\n";
        return Result<void, std::string>::ok();
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "set_quota";
    req["id"] = 1;
    req["params"]["name"] = manifest.name;
    req["params"]["limits"]["ram_mb"] = manifest.resource_contract.min_ram_mb;
    req["params"]["limits"]["vram_mb"] = manifest.resource_contract.min_vram_mb;
    req["params"]["limits"]["gpu_percent"] = manifest.resource_contract.gpu_compute_percent;
    req["params"]["limits"]["disk_mb"] = manifest.resource_contract.max_disk_mb;
    req["params"]["limits"]["cpu_cores"] = manifest.resource_contract.min_cpu_cores;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        std::cerr << "capsule: warning: quota registration failed: " << resp.error() << "\n";
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleInstaller::unregister_from_quota(const std::string& name) {
    IpcJsonClient client;
    auto conn = client.connect("/run/straylight/quota.sock");
    if (!conn.has_value()) {
        return Result<void, std::string>::ok();
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "remove_quota";
    req["id"] = 1;
    req["params"]["name"] = name;

    client.request(req);
    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleInstaller::create_desktop_entry(
    const CapsuleManifest& manifest,
    const std::filesystem::path& install_dir) {
    auto desktop_path = std::filesystem::path("/usr/share/applications") /
        ("straylight-capsule-" + manifest.name + ".desktop");

    std::ofstream ofs(desktop_path);
    if (!ofs.is_open()) {
        return Result<void, std::string>::error(
            "Cannot write desktop entry: " + desktop_path.string());
    }

    ofs << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=" << manifest.name << "\n"
        << "Comment=" << manifest.description << "\n"
        << "Exec=straylight-capsule run " << manifest.name << "\n"
        << "Terminal=false\n"
        << "Categories=StrayLight;\n"
        << "X-StrayLight-Capsule=true\n"
        << "X-StrayLight-Version=" << manifest.version << "\n";

    // Check for an icon in the capsule
    auto icon_path = install_dir / "icon.png";
    if (std::filesystem::exists(icon_path)) {
        ofs << "Icon=" << icon_path.string() << "\n";
    } else {
        ofs << "Icon=application-x-executable\n";
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleInstaller::remove_desktop_entry(const std::string& name) {
    auto desktop_path = std::filesystem::path("/usr/share/applications") /
        ("straylight-capsule-" + name + ".desktop");

    std::error_code ec;
    std::filesystem::remove(desktop_path, ec);
    // Don't fail if the file doesn't exist
    return Result<void, std::string>::ok();
}

Result<InstalledCapsule, std::string> CapsuleInstaller::install(
    const std::filesystem::path& capsule_path) {
    if (!std::filesystem::exists(capsule_path)) {
        return Result<InstalledCapsule, std::string>::error(
            "Capsule file not found: " + capsule_path.string());
    }

    std::cout << "capsule: verifying package integrity...\n";

    // Verify hash if sidecar exists
    auto hash_result = verify_hash(capsule_path);
    if (!hash_result.has_value()) {
        return Result<InstalledCapsule, std::string>::error(hash_result.error());
    }

    std::cout << "capsule: extracting archive...\n";

    // Extract to temporary location
    auto extract_result = extract_archive(capsule_path);
    if (!extract_result.has_value()) {
        return Result<InstalledCapsule, std::string>::error(extract_result.error());
    }
    auto extracted_dir = extract_result.value();

    // Load manifest from extracted contents
    auto manifest_path = extracted_dir / "capsule.json";
    if (!std::filesystem::exists(manifest_path)) {
        // Try one level deeper (tar might have wrapped in a directory)
        for (const auto& entry : std::filesystem::directory_iterator(extracted_dir)) {
            if (entry.is_directory() &&
                std::filesystem::exists(entry.path() / "capsule.json")) {
                extracted_dir = entry.path();
                manifest_path = extracted_dir / "capsule.json";
                break;
            }
        }
    }

    std::ifstream mf(manifest_path);
    if (!mf.is_open()) {
        return Result<InstalledCapsule, std::string>::error(
            "Cannot read capsule.json from extracted archive");
    }

    std::string manifest_str((std::istreambuf_iterator<char>(mf)),
                             std::istreambuf_iterator<char>());
    auto manifest_result = CapsuleManifestParser::parse(manifest_str);
    if (!manifest_result.has_value()) {
        return Result<InstalledCapsule, std::string>::error(manifest_result.error());
    }
    const auto& manifest = manifest_result.value();

    std::cout << "capsule: installing " << manifest.name << " v" << manifest.version << "\n";

    // Check system capabilities against resource contract
    auto cap_result = check_system_capabilities(manifest.resource_contract);
    if (!cap_result.has_value()) {
        return Result<InstalledCapsule, std::string>::error(cap_result.error());
    }

    // Install apt dependencies
    auto dep_result = install_dependencies(manifest.dependencies);
    if (!dep_result.has_value()) {
        return Result<InstalledCapsule, std::string>::error(dep_result.error());
    }

    // Move to final install location
    auto install_dir = std::filesystem::path(CAPSULE_BASE) / manifest.name;

    // Remove old installation if it exists
    if (std::filesystem::exists(install_dir)) {
        std::cout << "capsule: removing previous installation\n";
        std::filesystem::remove_all(install_dir);
    }

    std::filesystem::create_directories(install_dir.parent_path());

    std::error_code ec;
    std::filesystem::rename(extracted_dir, install_dir, ec);
    if (ec) {
        // rename across filesystems fails — fall back to copy
        std::filesystem::copy(extracted_dir, install_dir,
                             std::filesystem::copy_options::recursive, ec);
        if (ec) {
            return Result<InstalledCapsule, std::string>::error(
                "Failed to install to " + install_dir.string() + ": " + ec.message());
        }
        std::filesystem::remove_all(extracted_dir);
    }

    // Make binary executable
    auto binary_path = install_dir / manifest.binary_path;
    if (std::filesystem::exists(binary_path)) {
        std::filesystem::permissions(binary_path,
            std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
    }

    // Register with quota service
    auto quota_result = register_with_quota(manifest);
    if (!quota_result.has_value()) {
        std::cerr << "capsule: warning: " << quota_result.error() << "\n";
    }

    // Create desktop entry
    auto desktop_result = create_desktop_entry(manifest, install_dir);
    if (!desktop_result.has_value()) {
        std::cerr << "capsule: warning: " << desktop_result.error() << "\n";
    }

    // Clean up temp extraction directory
    auto tmp_dir = std::filesystem::temp_directory_path() / "capsule-extract";
    std::filesystem::remove_all(tmp_dir, ec);

    InstalledCapsule installed;
    installed.name = manifest.name;
    installed.version = manifest.version;
    installed.description = manifest.description;
    installed.install_path = install_dir.string();
    installed.resource_contract = manifest.resource_contract;

    std::cout << "capsule: " << manifest.name << " installed to " << install_dir.string() << "\n";
    return Result<InstalledCapsule, std::string>::ok(std::move(installed));
}

Result<void, std::string> CapsuleInstaller::uninstall(const std::string& name) {
    auto install_dir = std::filesystem::path(CAPSULE_BASE) / name;
    if (!std::filesystem::exists(install_dir)) {
        return Result<void, std::string>::error(
            "Capsule not installed: " + name);
    }

    std::cout << "capsule: uninstalling " << name << "\n";

    // Unregister from quota
    unregister_from_quota(name);

    // Remove desktop entry
    remove_desktop_entry(name);

    // Remove the installation directory
    std::error_code ec;
    std::filesystem::remove_all(install_dir, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "Failed to remove " + install_dir.string() + ": " + ec.message());
    }

    std::cout << "capsule: " << name << " uninstalled\n";
    return Result<void, std::string>::ok();
}

std::vector<InstalledCapsule> CapsuleInstaller::list_installed() {
    std::vector<InstalledCapsule> result;

    auto base = std::filesystem::path(CAPSULE_BASE);
    if (!std::filesystem::exists(base)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(base)) {
        if (!entry.is_directory()) continue;

        auto manifest_path = entry.path() / "capsule.json";
        if (!std::filesystem::exists(manifest_path)) continue;

        std::ifstream ifs(manifest_path);
        if (!ifs.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
        auto parsed = CapsuleManifestParser::parse(content);
        if (!parsed.has_value()) continue;

        const auto& m = parsed.value();
        InstalledCapsule installed;
        installed.name = m.name;
        installed.version = m.version;
        installed.description = m.description;
        installed.install_path = entry.path().string();
        installed.resource_contract = m.resource_contract;
        result.push_back(std::move(installed));
    }

    return result;
}

Result<InstalledCapsule, std::string> CapsuleInstaller::get_installed(const std::string& name) {
    auto install_dir = std::filesystem::path(CAPSULE_BASE) / name;
    auto manifest_path = install_dir / "capsule.json";

    if (!std::filesystem::exists(manifest_path)) {
        return Result<InstalledCapsule, std::string>::error(
            "Capsule not installed: " + name);
    }

    std::ifstream ifs(manifest_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
    auto parsed = CapsuleManifestParser::parse(content);
    if (!parsed.has_value()) {
        return Result<InstalledCapsule, std::string>::error(parsed.error());
    }

    const auto& m = parsed.value();
    InstalledCapsule installed;
    installed.name = m.name;
    installed.version = m.version;
    installed.description = m.description;
    installed.install_path = install_dir.string();
    installed.resource_contract = m.resource_contract;
    return Result<InstalledCapsule, std::string>::ok(std::move(installed));
}

} // namespace straylight
