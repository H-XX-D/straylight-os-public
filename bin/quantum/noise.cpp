// bin/quantum/noise.cpp
#include "noise.h"

#include <cmath>
#include <numeric>
#include <random>

namespace straylight::quantum {

void NoiseModel::add_depolarizing(double p) {
    // Depolarizing: E0 = sqrt(1-p)*I, E1 = sqrt(p/3)*X, E2 = sqrt(p/3)*Y, E3 = sqrt(p/3)*Z
    double s0 = std::sqrt(1.0 - p);
    double s1 = std::sqrt(p / 3.0);

    auto I = gates::identity();
    auto X = gates::pauli_x();
    auto Y = gates::pauli_y();
    auto Z = gates::pauli_z();

    // Scale matrices
    auto scale = [](Matrix2x2 m, double s) -> Matrix2x2 {
        for (auto& c : m) c *= s;
        return m;
    };

    NoiseChannel ch;
    ch.name = "depolarizing";
    ch.kraus_ops.push_back(scale(I, s0));
    ch.kraus_ops.push_back(scale(X, s1));
    ch.kraus_ops.push_back(scale(Y, s1));
    ch.kraus_ops.push_back(scale(Z, s1));
    channels_.push_back(std::move(ch));
}

void NoiseModel::add_dephasing(double p) {
    // Dephasing: E0 = sqrt(1-p)*I, E1 = sqrt(p)*Z
    double s0 = std::sqrt(1.0 - p);
    double s1 = std::sqrt(p);

    auto scale = [](Matrix2x2 m, double s) -> Matrix2x2 {
        for (auto& c : m) c *= s;
        return m;
    };

    NoiseChannel ch;
    ch.name = "dephasing";
    ch.kraus_ops.push_back(scale(gates::identity(), s0));
    ch.kraus_ops.push_back(scale(gates::pauli_z(), s1));
    channels_.push_back(std::move(ch));
}

void NoiseModel::add_amplitude_damping(double gamma) {
    // E0 = [[1, 0], [0, sqrt(1-gamma)]]
    // E1 = [[0, sqrt(gamma)], [0, 0]]
    double sg = std::sqrt(gamma);
    double s1g = std::sqrt(1.0 - gamma);

    NoiseChannel ch;
    ch.name = "amplitude_damping";
    ch.kraus_ops.push_back({Complex{1, 0}, Complex{0, 0},
                            Complex{0, 0}, Complex{s1g, 0}});
    ch.kraus_ops.push_back({Complex{0, 0}, Complex{sg, 0},
                            Complex{0, 0}, Complex{0, 0}});
    channels_.push_back(std::move(ch));
}

Result<void, std::string> NoiseModel::apply(StateVector& sv, uint32_t qubit) {
    if (qubit >= sv.num_qubits()) {
        return Result<void, std::string>::error(
            "Qubit " + std::to_string(qubit) + " out of range");
    }

    // Thread-local RNG for sampling.
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    for (const auto& ch : channels_) {
        // For each Kraus channel, we sample which operator to apply.
        // The probability of operator K_i is Tr(K_i rho K_i^dag).
        // We compute this by applying each K_i to a copy and measuring norm^2.
        std::vector<double> probs(ch.kraus_ops.size());
        std::vector<StateVector> copies;
        copies.reserve(ch.kraus_ops.size());

        for (size_t i = 0; i < ch.kraus_ops.size(); ++i) {
            copies.push_back(sv); // copy
            apply_kraus(copies.back(), qubit, ch.kraus_ops[i]);
            double norm_sq = 0.0;
            for (size_t j = 0; j < copies.back().dim(); ++j) {
                norm_sq += std::norm(copies.back()[j]);
            }
            probs[i] = norm_sq;
        }

        // Normalize probabilities.
        double total = std::accumulate(probs.begin(), probs.end(), 0.0);
        if (total < 1e-15) {
            return Result<void, std::string>::error(
                "Noise channel '" + ch.name + "' produced zero total probability");
        }
        for (auto& p : probs) p /= total;

        // Sample an operator.
        std::discrete_distribution<size_t> dist(probs.begin(), probs.end());
        size_t chosen = dist(rng);

        // Replace state with the chosen post-measurement state, renormalized.
        sv = std::move(copies[chosen]);
        double norm = 0.0;
        for (size_t j = 0; j < sv.dim(); ++j) {
            norm += std::norm(sv[j]);
        }
        double inv_norm = 1.0 / std::sqrt(norm);
        for (size_t j = 0; j < sv.dim(); ++j) {
            sv[j] *= inv_norm;
        }
    }

    return Result<void, std::string>::ok();
}

void NoiseModel::apply_kraus(StateVector& sv, uint32_t qubit, const Matrix2x2& mat) {
    size_t n = sv.dim();
    uint32_t nq = sv.num_qubits();
    size_t step = static_cast<size_t>(1) << (nq - 1 - qubit);
    for (size_t block = 0; block < n; block += step * 2) {
        for (size_t i = 0; i < step; ++i) {
            size_t idx0 = block + i;
            size_t idx1 = block + i + step;
            auto a0 = sv[idx0];
            auto a1 = sv[idx1];
            sv[idx0] = mat[0] * a0 + mat[1] * a1;
            sv[idx1] = mat[2] * a0 + mat[3] * a1;
        }
    }
}

} // namespace straylight::quantum
