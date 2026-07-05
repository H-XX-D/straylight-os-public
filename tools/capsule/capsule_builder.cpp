// tools/capsule/capsule_builder.cpp
#include "capsule_builder.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace straylight {

Result<CapsuleManifest, std::string> CapsuleBuilder::load_manifest(
    const std::filesystem::path& dir) {
    auto manifest_path = dir / "capsule.json";
    if (!std::filesystem::exists(manifest_path)) {
        return Result<CapsuleManifest, std::string>::error(
            "No capsule.json found in " + dir.string());
    }

    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) {
        return Result<CapsuleManifest, std::string>::error(
            "Cannot open " + manifest_path.string());
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    return CapsuleManifestParser::parse(content);
}

Result<void, std::string> CapsuleBuilder::verify_contents(
    const CapsuleManifest& manifest,
    const std::filesystem::path& dir) {
    // Check that the binary exists
    auto binary = dir / manifest.binary_path;
    if (!std::filesystem::exists(binary)) {
        return Result<void, std::string>::error(
            "Binary not found: " + manifest.binary_path +
            " (looked in " + binary.string() + ")");
    }

    // Check that it's actually a file (not a directory)
    if (!std::filesystem::is_regular_file(binary)) {
        return Result<void, std::string>::error(
            "Binary path is not a regular file: " + binary.string());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleBuilder::create_archive(
    const std::filesystem::path& dir,
    const std::filesystem::path& output) {
    // Use tar with zstd compression
    // tar -C <parent> -cf - <dirname> | zstd -o <output>
    std::string parent = dir.parent_path().string();
    std::string dirname = dir.filename().string();
    std::string out = output.string();

    // First try tar with --zstd flag (GNU tar)
    std::string cmd = "tar --zstd -cf '" + out + "' -C '" + parent + "' '" + dirname + "' 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to execute tar command");
    }

    std::string tar_output;
    std::array<char, 256> buf;
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        tar_output += buf.data();
    }
    int status = ::pclose(pipe);

    if (status != 0) {
        // Fallback: tar + pipe to zstd
        std::string fallback_cmd = "tar -cf - -C '" + parent + "' '" + dirname +
                                   "' | zstd -q -o '" + out + "' 2>&1";
        pipe = ::popen(fallback_cmd.c_str(), "r");
        if (!pipe) {
            return Result<void, std::string>::error("Failed to execute tar|zstd command");
        }
        tar_output.clear();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            tar_output += buf.data();
        }
        status = ::pclose(pipe);
        if (status != 0) {
            return Result<void, std::string>::error(
                "Archive creation failed: " + tar_output);
        }
    }

    if (!std::filesystem::exists(output)) {
        return Result<void, std::string>::error(
            "Archive was not created at " + output.string());
    }

    return Result<void, std::string>::ok();
}

Result<std::string, std::string> CapsuleBuilder::sign_package(
    const std::filesystem::path& archive_path) {
    // Compute SHA-256 hash
    std::string cmd = "sha256sum '" + archive_path.string() + "' 2>/dev/null || " +
                      "shasum -a 256 '" + archive_path.string() + "' 2>/dev/null";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error("Failed to execute sha256sum");
    }

    std::string output;
    std::array<char, 256> buf;
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    int status = ::pclose(pipe);

    if (status != 0 || output.empty()) {
        return Result<std::string, std::string>::error("SHA-256 computation failed");
    }

    // Extract just the hash (first 64 hex chars)
    std::string hash;
    for (char c : output) {
        if (std::isxdigit(c) && hash.size() < 64) {
            hash += c;
        } else if (hash.size() == 64) {
            break;
        }
    }

    if (hash.size() != 64) {
        return Result<std::string, std::string>::error(
            "Invalid SHA-256 hash length: " + std::to_string(hash.size()));
    }

    // Write sidecar file
    auto sidecar_path = archive_path.string() + ".sha256";
    std::ofstream ofs(sidecar_path);
    if (!ofs.is_open()) {
        return Result<std::string, std::string>::error(
            "Cannot write hash sidecar: " + sidecar_path);
    }
    ofs << hash << "  " << archive_path.filename().string() << "\n";
    ofs.close();

    return Result<std::string, std::string>::ok(std::move(hash));
}

Result<std::string, std::string> CapsuleBuilder::build(const std::filesystem::path& directory) {
    auto manifest_result = load_manifest(directory);
    if (!manifest_result.has_value()) {
        return Result<std::string, std::string>::error(manifest_result.error());
    }
    const auto& manifest = manifest_result.value();

    auto output_path = directory.parent_path() /
        (manifest.name + "-" + manifest.version + ".capsule");
    return build(directory, output_path);
}

Result<std::string, std::string> CapsuleBuilder::build(
    const std::filesystem::path& directory,
    const std::filesystem::path& output_path) {
    if (!std::filesystem::exists(directory)) {
        return Result<std::string, std::string>::error(
            "Directory does not exist: " + directory.string());
    }

    if (!std::filesystem::is_directory(directory)) {
        return Result<std::string, std::string>::error(
            "Not a directory: " + directory.string());
    }

    // Load and validate manifest
    auto manifest_result = load_manifest(directory);
    if (!manifest_result.has_value()) {
        return Result<std::string, std::string>::error(manifest_result.error());
    }
    const auto& manifest = manifest_result.value();

    std::cout << "capsule: building " << manifest.name << " v" << manifest.version << "\n";

    // Verify all referenced content exists
    auto verify_result = verify_contents(manifest, directory);
    if (!verify_result.has_value()) {
        return Result<std::string, std::string>::error(verify_result.error());
    }

    std::cout << "capsule: verified manifest and contents\n";

    // Create the archive
    auto archive_result = create_archive(directory, output_path);
    if (!archive_result.has_value()) {
        return Result<std::string, std::string>::error(archive_result.error());
    }

    auto size = std::filesystem::file_size(output_path);
    std::cout << "capsule: archive created (" << size << " bytes)\n";

    // Sign with SHA-256
    auto sign_result = sign_package(output_path);
    if (!sign_result.has_value()) {
        return Result<std::string, std::string>::error(sign_result.error());
    }

    std::cout << "capsule: signed (sha256: " << sign_result.value().substr(0, 16) << "...)\n";

    // Print dependency summary
    if (!manifest.dependencies.empty()) {
        std::cout << "capsule: " << manifest.dependencies.size() << " apt dependencies: ";
        for (size_t i = 0; i < manifest.dependencies.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << manifest.dependencies[i];
        }
        std::cout << "\n";
    }

    // Print resource contract summary
    const auto& rc = manifest.resource_contract;
    std::cout << "capsule: resource contract — "
              << "RAM≥" << rc.min_ram_mb << "MB"
              << " VRAM≥" << rc.min_vram_mb << "MB"
              << " GPU≤" << rc.gpu_compute_percent << "%"
              << " CPU≥" << rc.min_cpu_cores << " cores"
              << " disk≤" << rc.max_disk_mb << "MB"
              << (rc.requires_mesh ? " +mesh" : "")
              << (rc.requires_network ? " +net" : "")
              << "\n";

    return Result<std::string, std::string>::ok(output_path.string());
}

} // namespace straylight
