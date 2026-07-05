// bin/photonics/device.h
// Hardware abstraction for photonic processors (simulated)
#pragma once

#include "detector.h"
#include "mesh.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::photonics {

class PhotonicDevice {
public:
    /// Connect to a device (simulated: just validates path and sets connected state).
    Result<void, std::string> connect(const std::string& device_path);

    /// Program the mesh onto the device.
    Result<void, std::string> program_mesh(const PhotonicMesh& mesh);

    /// Run input through the programmed mesh and detect output.
    Result<std::vector<double>, std::string>
    run(const std::vector<Complex>& input);

    [[nodiscard]] bool is_connected() const { return connected_; }

private:
    bool connected_ = false;
    std::string device_path_;
    PhotonicMesh mesh_;
    Detector detector_;
    double efficiency_ = 0.95;
    double dark_count_rate_ = 1e-6;
};

} // namespace straylight::photonics
