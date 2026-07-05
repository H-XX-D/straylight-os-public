// bin/dpdk/flow.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::dpdk {

/// Action to take on a classified flow.
enum class FlowAction : uint8_t {
    Pass    = 0,   // Forward normally
    Drop    = 1,   // Drop the packet
    Mirror  = 2,   // Mirror to a second port
    Mark    = 3,   // Mark for QoS
    Redirect = 4,  // Redirect to another queue/port
};

/// A flow classification rule.
struct FlowRule {
    uint32_t src_ip     = 0;  // Source IP (network byte order), 0 = wildcard
    uint32_t dst_ip     = 0;  // Destination IP (network byte order), 0 = wildcard
    uint16_t src_port   = 0;  // Source port, 0 = wildcard
    uint16_t dst_port   = 0;  // Destination port, 0 = wildcard
    uint8_t  protocol   = 0;  // IP protocol (6=TCP, 17=UDP), 0 = wildcard
    FlowAction action   = FlowAction::Pass;
    uint16_t redirect_target = 0; // target port/queue for Redirect action
    uint32_t mark_value = 0;      // mark value for Mark action
};

/// Classifies packets against installed flow rules.
class FlowClassifier {
public:
    FlowClassifier() = default;

    /// Add a flow rule. Returns a unique rule ID.
    Result<uint64_t, std::string> add_rule(const FlowRule& rule);

    /// Remove a rule by ID.
    Result<void, std::string> remove_rule(uint64_t id);

    /// Classify a packet described by its 5-tuple. Returns the matching action.
    Result<FlowAction, std::string> classify(uint32_t src_ip, uint32_t dst_ip,
                                             uint16_t src_port, uint16_t dst_port,
                                             uint8_t protocol) const;

    /// Number of installed rules.
    [[nodiscard]] size_t rule_count() const noexcept;

    /// Get a rule by ID, if it exists.
    [[nodiscard]] const FlowRule* get_rule(uint64_t id) const;

private:
    struct InstalledRule {
        uint64_t id;
        FlowRule rule;
    };

    std::vector<InstalledRule> rules_;
    uint64_t next_id_ = 1;

    /// Check whether a packet 5-tuple matches a rule (wildcard-aware).
    static bool matches(const FlowRule& rule,
                        uint32_t src_ip, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint8_t protocol);
};

} // namespace straylight::dpdk
