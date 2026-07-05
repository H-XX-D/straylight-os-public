// bin/dpdk/flow.cpp
#include "flow.h"

#include <algorithm>

namespace straylight::dpdk {

Result<uint64_t, std::string> FlowClassifier::add_rule(const FlowRule& rule) {
    // Validate: at least one field must be non-wildcard or we accept a catch-all
    // (a catch-all with all zeros is valid — it matches everything)

    uint64_t id = next_id_++;
    rules_.push_back(InstalledRule{id, rule});
    return Result<uint64_t, std::string>::ok(id);
}

Result<void, std::string> FlowClassifier::remove_rule(uint64_t id) {
    auto it = std::find_if(rules_.begin(), rules_.end(),
                           [id](const InstalledRule& r) { return r.id == id; });
    if (it == rules_.end()) {
        return Result<void, std::string>::error(
            "Rule ID " + std::to_string(id) + " not found");
    }

    rules_.erase(it);
    return Result<void, std::string>::ok();
}

Result<FlowAction, std::string> FlowClassifier::classify(uint32_t src_ip, uint32_t dst_ip,
                                                          uint16_t src_port, uint16_t dst_port,
                                                          uint8_t protocol) const {
    // First matching rule wins (priority = insertion order)
    for (const auto& installed : rules_) {
        if (matches(installed.rule, src_ip, dst_ip, src_port, dst_port, protocol)) {
            return Result<FlowAction, std::string>::ok(installed.rule.action);
        }
    }

    // No rule matched — default to pass
    return Result<FlowAction, std::string>::ok(FlowAction::Pass);
}

size_t FlowClassifier::rule_count() const noexcept {
    return rules_.size();
}

const FlowRule* FlowClassifier::get_rule(uint64_t id) const {
    for (const auto& installed : rules_) {
        if (installed.id == id) {
            return &installed.rule;
        }
    }
    return nullptr;
}

bool FlowClassifier::matches(const FlowRule& rule,
                              uint32_t src_ip, uint32_t dst_ip,
                              uint16_t src_port, uint16_t dst_port,
                              uint8_t protocol) {
    // A field value of 0 in the rule means "wildcard" — match anything
    if (rule.src_ip != 0 && rule.src_ip != src_ip) {
        return false;
    }
    if (rule.dst_ip != 0 && rule.dst_ip != dst_ip) {
        return false;
    }
    if (rule.src_port != 0 && rule.src_port != src_port) {
        return false;
    }
    if (rule.dst_port != 0 && rule.dst_port != dst_port) {
        return false;
    }
    if (rule.protocol != 0 && rule.protocol != protocol) {
        return false;
    }
    return true;
}

} // namespace straylight::dpdk
