// bin/photonics/mesh.cpp
#include "mesh.h"

namespace straylight::photonics {

void PhotonicMesh::set_size(uint32_t rows, uint32_t cols) {
    rows_ = rows;
    cols_ = cols;
    // Each column has floor(rows/2) MZI units (pairing adjacent waveguides).
    // Odd columns offset by 1 (Clements-style interleaving).
    mzis_.resize(cols);
    for (uint32_t c = 0; c < cols; ++c) {
        uint32_t offset = (c % 2 == 0) ? 0 : 1;
        uint32_t num_mzis = (rows - offset) / 2;
        mzis_[c].resize(num_mzis, MZI{0.0, 0.0});
    }
}

void PhotonicMesh::set_mzi(uint32_t row, uint32_t col, MZI params) {
    if (col >= cols_) return;
    uint32_t offset = (col % 2 == 0) ? 0 : 1;
    uint32_t idx = (row - offset) / 2;
    if (idx < mzis_[col].size()) {
        mzis_[col][idx] = params;
    }
}

Result<std::vector<Complex>, std::string>
PhotonicMesh::forward(const std::vector<Complex>& input) const {
    if (input.size() != rows_) {
        return Result<std::vector<Complex>, std::string>::error(
            "Input size " + std::to_string(input.size()) +
            " does not match mesh rows " + std::to_string(rows_));
    }
    if (rows_ == 0 || cols_ == 0) {
        return Result<std::vector<Complex>, std::string>::error("Mesh not initialized");
    }

    std::vector<Complex> state = input;

    // Apply column by column.
    for (uint32_t c = 0; c < cols_; ++c) {
        uint32_t offset = (c % 2 == 0) ? 0 : 1;
        for (uint32_t m = 0; m < mzis_[c].size(); ++m) {
            uint32_t w0 = offset + m * 2;
            uint32_t w1 = w0 + 1;
            if (w1 >= rows_) break;

            auto result = unit_.propagate(state[w0], state[w1], mzis_[c][m]);
            if (!result.has_value()) {
                return Result<std::vector<Complex>, std::string>::error(result.error());
            }
            state[w0] = result.value().first;
            state[w1] = result.value().second;
        }
    }

    return Result<std::vector<Complex>, std::string>::ok(std::move(state));
}

} // namespace straylight::photonics
