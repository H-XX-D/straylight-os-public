// tools/capsule/capsule_builder.h
// Build capsule packages — tar.zst archives with manifest + binary + assets.
#pragma once

#include "capsule_manifest.h"

#include <straylight/result.h>

#include <filesystem>
#include <string>

namespace straylight {

/// Builds .capsule archives (tar.zst) from an application directory.
class CapsuleBuilder {
public:
    /// Build a .capsule archive from a directory containing capsule.json + app files.
    /// Returns the path to the generated .capsule file.
    static Result<std::string, std::string> build(const std::filesystem::path& directory);

    /// Build with an explicit output path.
    static Result<std::string, std::string> build(const std::filesystem::path& directory,
                                                   const std::filesystem::path& output_path);

private:
    /// Read and validate the manifest from the directory.
    static Result<CapsuleManifest, std::string> load_manifest(const std::filesystem::path& dir);

    /// Create a tar.zst archive of the directory.
    static Result<void, std::string> create_archive(const std::filesystem::path& dir,
                                                     const std::filesystem::path& output);

    /// Compute SHA-256 hash of the archive and write .sha256 sidecar.
    static Result<std::string, std::string> sign_package(const std::filesystem::path& archive_path);

    /// Verify all referenced files in manifest exist in the directory.
    static Result<void, std::string> verify_contents(const CapsuleManifest& manifest,
                                                      const std::filesystem::path& dir);
};

} // namespace straylight
