// bin/scheduler/topology.cpp
#include "topology.h"

#include <set>
#include <sstream>
#include <string>

namespace straylight {

Result<void, SLError> Topology::parse_cpuinfo(const std::string& content) {
    if (content.empty()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, "empty cpuinfo content"});
    }

    unsigned logical = 0;
    std::set<std::string> physical_cores;

    std::string current_physical_id;
    std::string current_core_id;

    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip empty lines — they delimit processor blocks
        if (line.empty() || line.find_first_not_of(" \t\n\r") == std::string::npos) {
            // End of a processor block: record physical core if we have ids
            if (!current_physical_id.empty() && !current_core_id.empty()) {
                physical_cores.insert(current_physical_id + ":" + current_core_id);
            }
            current_physical_id.clear();
            current_core_id.clear();
            continue;
        }

        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string key = line.substr(0, colon_pos);
        std::string val = line.substr(colon_pos + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            auto start = s.find_first_not_of(" \t");
            auto end = s.find_last_not_of(" \t");
            s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };
        trim(key);
        trim(val);

        if (key == "processor") {
            ++logical;
        } else if (key == "physical id") {
            current_physical_id = val;
        } else if (key == "core id") {
            current_core_id = val;
        }
    }

    // Handle last block (file may not end with blank line)
    if (!current_physical_id.empty() && !current_core_id.empty()) {
        physical_cores.insert(current_physical_id + ":" + current_core_id);
    }

    if (logical == 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, "no processors found in cpuinfo"});
    }

    logical_count_ = logical;
    physical_cores_ = static_cast<unsigned>(physical_cores.size());

    return Result<void, SLError>::ok();
}

} // namespace straylight
