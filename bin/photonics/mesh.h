// bin/photonics/mesh.h
// Photonic mesh: Clements/Reck architecture of MZI units
#pragma once

#include "mzi.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::photonics {

class PhotonicMesh {
public:
    /// Set the mesh dimensions (rows = waveguides, cols = MZI columns).
    void set_size(uint32_t rows, uint32_t cols);

    /// Set MZI parameters at a given position.
    void set_mzi(uint32_t row, uint32_t col, MZI params);

    /// Forward propagation: apply column-by-column MZI operations.
    /// Input size must match the number of rows.
    Result<std::vector<Complex>, std::string>
    forward(const std::vector<Complex>& input) const;

    [[nodiscard]] uint32_t rows() const { return rows_; }
    [[nodiscard]] uint32_t cols() const { return cols_; }

private:
    uint32_t rows_ = 0;
    uint32_t cols_ = 0;
    // 2D grid: mzis_[col][row/2] — each MZI couples adjacent waveguide pairs.
    std::vector<std::vector<MZI>> mzis_;
    MZIUnit unit_;
};

} // namespace straylight::photonics
