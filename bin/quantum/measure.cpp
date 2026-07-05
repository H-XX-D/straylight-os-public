// bin/quantum/measure.cpp
#include "measure.h"

#include <cmath>
#include <numeric>
#include <random>

namespace straylight::quantum {

Result<uint32_t, std::string> Measurement::measure_qubit(StateVector& sv, uint32_t qubit) {
    if (qubit >= sv.num_qubits()) {
        return Result<uint32_t, std::string>::error(
            "Qubit " + std::to_string(qubit) + " out of range [0," +
            std::to_string(sv.num_qubits()) + ")");
    }

    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    uint32_t nq = sv.num_qubits();
    size_t n = sv.dim();
    size_t bit_mask = static_cast<size_t>(1) << (nq - 1 - qubit);

    // Compute probability of measuring |0> on this qubit.
    double prob0 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if ((i & bit_mask) == 0) {
            prob0 += std::norm(sv[i]);
        }
    }

    double r = dist(rng);
    uint32_t outcome = (r < prob0) ? 0 : 1;

    // Collapse: zero out amplitudes inconsistent with outcome, renormalize.
    double norm_sq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        bool qubit_is_one = (i & bit_mask) != 0;
        if ((outcome == 0 && qubit_is_one) || (outcome == 1 && !qubit_is_one)) {
            sv[i] = {0.0, 0.0};
        } else {
            norm_sq += std::norm(sv[i]);
        }
    }

    // Renormalize.
    if (norm_sq < 1e-15) {
        return Result<uint32_t, std::string>::error(
            "Measurement produced zero-norm state (numerical error)");
    }
    double inv_norm = 1.0 / std::sqrt(norm_sq);
    for (size_t i = 0; i < n; ++i) {
        sv[i] *= inv_norm;
    }

    return Result<uint32_t, std::string>::ok(outcome);
}

Result<std::vector<uint32_t>, std::string> Measurement::measure_all(StateVector& sv,
                                                                      uint32_t shots) {
    if (shots == 0) {
        return Result<std::vector<uint32_t>, std::string>::error("shots must be > 0");
    }

    static thread_local std::mt19937_64 rng{std::random_device{}()};

    auto probs = sv.probabilities();
    size_t n = probs.size();

    // Build CDF for sampling.
    std::vector<double> cdf(n);
    cdf[0] = probs[0];
    for (size_t i = 1; i < n; ++i) {
        cdf[i] = cdf[i - 1] + probs[i];
    }
    // Normalize CDF end to exactly 1.0 for numerical safety.
    if (cdf.back() > 0.0) {
        double inv = 1.0 / cdf.back();
        for (auto& c : cdf) c *= inv;
    }
    cdf.back() = 1.0;

    std::vector<uint32_t> results(shots);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (uint32_t s = 0; s < shots; ++s) {
        double r = dist(rng);
        // Binary search for the sampled basis state.
        auto it = std::lower_bound(cdf.begin(), cdf.end(), r);
        size_t idx = static_cast<size_t>(std::distance(cdf.begin(), it));
        if (idx >= n) idx = n - 1;
        results[s] = static_cast<uint32_t>(idx);
    }

    return Result<std::vector<uint32_t>, std::string>::ok(std::move(results));
}

double Measurement::expectation(const StateVector& sv,
                                const std::vector<double>& observable) {
    double exp_val = 0.0;
    size_t n = std::min(sv.dim(), observable.size());
    for (size_t i = 0; i < n; ++i) {
        exp_val += std::norm(sv[i]) * observable[i];
    }
    return exp_val;
}

} // namespace straylight::quantum
