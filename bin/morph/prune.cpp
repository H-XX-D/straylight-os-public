// bin/morph/prune.cpp
#include "prune.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace straylight::morph {

static float compute_sparsity(const std::vector<bool>& mask) {
    size_t zeroed = 0;
    for (bool kept : mask) {
        if (!kept) ++zeroed;
    }
    return static_cast<float>(zeroed) / static_cast<float>(mask.size());
}

Result<PruneResult, std::string> Pruner::prune_magnitude(
    const std::vector<float>& weights, float sparsity) {

    size_t n = weights.size();
    size_t num_to_prune = static_cast<size_t>(std::round(sparsity * static_cast<float>(n)));
    num_to_prune = std::min(num_to_prune, n);

    // Build indices sorted by absolute magnitude (ascending)
    std::vector<size_t> indices(n);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return std::fabs(weights[a]) < std::fabs(weights[b]);
    });

    // The smallest num_to_prune weights get zeroed
    std::vector<bool> mask(n, true);
    for (size_t i = 0; i < num_to_prune; ++i) {
        mask[indices[i]] = false;
    }

    std::vector<float> pruned(n);
    for (size_t i = 0; i < n; ++i) {
        pruned[i] = mask[i] ? weights[i] : 0.0f;
    }

    PruneResult result;
    result.pruned = std::move(pruned);
    result.mask = std::move(mask);
    result.actual_sparsity = compute_sparsity(result.mask);
    return Result<PruneResult, std::string>::ok(std::move(result));
}

Result<PruneResult, std::string> Pruner::prune_structured(
    const std::vector<float>& weights, float sparsity, uint32_t cols) {

    if (cols == 0) {
        return Result<PruneResult, std::string>::error(
            "Structured pruning requires structured_cols > 0");
    }

    size_t n = weights.size();
    if (n % cols != 0) {
        return Result<PruneResult, std::string>::error(
            "Weight vector size must be divisible by structured_cols");
    }

    size_t rows = n / cols;
    size_t rows_to_prune = static_cast<size_t>(
        std::round(sparsity * static_cast<float>(rows)));
    rows_to_prune = std::min(rows_to_prune, rows);

    // Compute L2 norm of each row
    std::vector<std::pair<float, size_t>> row_norms(rows);
    for (size_t r = 0; r < rows; ++r) {
        float sum_sq = 0.0f;
        for (uint32_t c = 0; c < cols; ++c) {
            float v = weights[r * cols + c];
            sum_sq += v * v;
        }
        row_norms[r] = {std::sqrt(sum_sq), r};
    }

    // Sort by norm ascending — smallest norms get pruned
    std::sort(row_norms.begin(), row_norms.end());

    std::vector<bool> row_pruned(rows, false);
    for (size_t i = 0; i < rows_to_prune; ++i) {
        row_pruned[row_norms[i].second] = true;
    }

    std::vector<float> pruned(n);
    std::vector<bool> mask(n, true);
    for (size_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            size_t idx = r * cols + c;
            if (row_pruned[r]) {
                pruned[idx] = 0.0f;
                mask[idx] = false;
            } else {
                pruned[idx] = weights[idx];
            }
        }
    }

    PruneResult result;
    result.pruned = std::move(pruned);
    result.mask = std::move(mask);
    result.actual_sparsity = compute_sparsity(result.mask);
    return Result<PruneResult, std::string>::ok(std::move(result));
}

Result<PruneResult, std::string> Pruner::prune_random(
    const std::vector<float>& weights, float sparsity, uint64_t seed) {

    size_t n = weights.size();
    size_t num_to_prune = static_cast<size_t>(std::round(sparsity * static_cast<float>(n)));
    num_to_prune = std::min(num_to_prune, n);

    // Generate shuffled indices, prune the first num_to_prune
    std::vector<size_t> indices(n);
    std::iota(indices.begin(), indices.end(), 0);

    std::mt19937_64 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);

    std::vector<bool> mask(n, true);
    for (size_t i = 0; i < num_to_prune; ++i) {
        mask[indices[i]] = false;
    }

    std::vector<float> pruned(n);
    for (size_t i = 0; i < n; ++i) {
        pruned[i] = mask[i] ? weights[i] : 0.0f;
    }

    PruneResult result;
    result.pruned = std::move(pruned);
    result.mask = std::move(mask);
    result.actual_sparsity = compute_sparsity(result.mask);
    return Result<PruneResult, std::string>::ok(std::move(result));
}

Result<PruneResult, std::string> Pruner::prune(
    const std::vector<float>& weights, PruneConfig cfg) {

    if (weights.empty()) {
        return Result<PruneResult, std::string>::error(
            "Cannot prune empty weight vector");
    }

    if (cfg.sparsity < 0.0f || cfg.sparsity > 1.0f) {
        return Result<PruneResult, std::string>::error(
            "Sparsity must be in [0, 1]");
    }

    switch (cfg.strategy) {
    case PruneStrategy::Magnitude:
        return prune_magnitude(weights, cfg.sparsity);
    case PruneStrategy::Structured:
        return prune_structured(weights, cfg.sparsity, cfg.structured_cols);
    case PruneStrategy::Random:
        return prune_random(weights, cfg.sparsity, cfg.seed);
    }

    return Result<PruneResult, std::string>::error("Unknown prune strategy");
}

} // namespace straylight::morph
