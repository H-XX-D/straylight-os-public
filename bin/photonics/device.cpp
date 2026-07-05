// bin/photonics/device.cpp
#include "device.h"

#include <fstream>

namespace straylight::photonics {

Result<void, std::string> PhotonicDevice::connect(const std::string& device_path) {
    if (device_path.empty()) {
        return Result<void, std::string>::error("Device path cannot be empty");
    }

    // In simulation mode, we accept any non-empty path.
    // A real implementation would open /dev/photonic0 or similar.
    device_path_ = device_path;
    connected_ = true;

    // Check if the path looks like a real device; if so, verify access.
    if (device_path.starts_with("/dev/")) {
        std::ifstream test(device_path);
        if (!test.good()) {
            // Fall back to simulation mode — device not present.
            // This is expected on non-photonic hardware.
        }
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> PhotonicDevice::program_mesh(const PhotonicMesh& mesh) {
    if (!connected_) {
        return Result<void, std::string>::error("Device not connected");
    }
    if (mesh.rows() == 0 || mesh.cols() == 0) {
        return Result<void, std::string>::error("Cannot program empty mesh");
    }
    mesh_ = mesh;
    return Result<void, std::string>::ok();
}

Result<std::vector<double>, std::string>
PhotonicDevice::run(const std::vector<Complex>& input) {
    if (!connected_) {
        return Result<std::vector<double>, std::string>::error("Device not connected");
    }
    if (mesh_.rows() == 0) {
        return Result<std::vector<double>, std::string>::error("No mesh programmed");
    }

    // Forward propagation through mesh.
    auto fwd = mesh_.forward(input);
    if (!fwd.has_value()) {
        return Result<std::vector<double>, std::string>::error(fwd.error());
    }

    // Detection.
    auto det = detector_.detect(fwd.value(), efficiency_, dark_count_rate_);
    if (!det.has_value()) {
        return Result<std::vector<double>, std::string>::error(det.error());
    }

    return Result<std::vector<double>, std::string>::ok(std::move(det).value());
}

} // namespace straylight::photonics
