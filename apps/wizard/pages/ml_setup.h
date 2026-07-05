// apps/wizard/pages/ml_setup.h
// ML framework detection, GPU profile setup, and dataset storage drive pick
#pragma once

#include <string>
#include <vector>

namespace straylight::wizard {

/// Detected ML framework.
struct MlFramework {
    std::string name;
    bool present = false;
};

/// A block device that can be designated as the ML data store.
struct StorageDrive {
    std::string path;    // "/dev/nvme1n1" or a mount-point directory
    std::string label;   // filesystem label or model name
    std::string size;    // human-readable  ("1.8 TiB")
    bool        mounted; // already has a mount point
    std::string mountpt; // existing mount point if mounted
};

/// ML setup page — detect installed frameworks, configure GPU profile,
/// and designate a drive / directory for datasets and models.
class MlSetupPage {
public:
    MlSetupPage();
    ~MlSetupPage() = default;

    /// Render the page. Returns true to advance.
    bool render();

    /// Detect installed ML frameworks.
    void detect_frameworks();

    /// Enumerate available storage drives (called lazily on first render).
    void enumerate_storage();

    /// Get detected frameworks.
    [[nodiscard]] const std::vector<MlFramework>& frameworks() const {
        return frameworks_;
    }

    /// Get selected GPU profile name.
    [[nodiscard]] const std::string& gpu_profile() const {
        return gpu_profile_;
    }

    /// Path where datasets/models will be stored.  May be a directory on an
    /// existing mount, or the raw block device path if not yet mounted.
    [[nodiscard]] const std::string& data_store_path() const {
        return data_store_path_;
    }

private:
    std::vector<MlFramework> frameworks_;
    std::string gpu_vendor_;
    std::string gpu_profile_ = "balanced";
    int  profile_index_     = 1;
    bool detected_          = false;

    std::vector<StorageDrive> storage_drives_;
    int  storage_index_     = 0;   // 0 = home directory
    bool storage_enumerated_= false;
    std::string data_store_path_;

    void write_data_store_config();
};

} // namespace straylight::wizard
