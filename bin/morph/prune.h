// bin/morph/prune.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <straylight/result.h>

namespace straylight::morph {

enum class PruneStrategy { Magnitude, Structured, Random };

struct PruneConfig {
    PruneStrategy strategy = PruneStrategy::Magnitude;
    float sparsity = 0.5f;
    /// For structured pruning: number of columns per row (matrix width).
    /// The weight vector is treated as a matrix of shape [rows x cols].
    uint32_t structured_cols = 0;
    /// Random seed for reproducible random pruning.
    uint64_t seed = 42;
};

struct PruneResult {
    std::vector<float> pruned;
    std::vector<bool> mask;  // true = kept, false = pruned
    float actual_sparsity;
};

class Pruner {
public:
    /// Prune weights according to cfg, returning pruned weights and a mask.
    Result<PruneResult, std::string> prune(const std::vector<float>& weights,
                                           PruneConfig cfg);

private:
    Result<PruneResult, std::string> prune_magnitude(const std::vector<float>& weights,
                                                     float sparsity);
    Result<PruneResult, std::string> prune_structured(const std::vector<float>& weights,
                                                      float sparsity, uint32_t cols);
    Result<PruneResult, std::string> prune_random(const std::vector<float>& weights,
                                                  float sparsity, uint64_t seed);
};

} // namespace straylight::morph
