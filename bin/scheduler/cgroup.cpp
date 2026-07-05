// bin/scheduler/cgroup.cpp
#include "cgroup.h"

#include <fstream>
#include <string>

namespace straylight {

Result<unsigned, SLError> CgroupV2::read_cpu_weight() const {
    auto file_path = path_ / "cpu.weight";
    std::ifstream f(file_path);
    if (!f.is_open()) {
        return Result<unsigned, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "cannot open " + file_path.string()});
    }

    unsigned weight = 0;
    f >> weight;
    if (f.fail()) {
        return Result<unsigned, SLError>::error(
            SLError{SLErrorCode::ParseError,
                    "failed to parse cpu.weight from " + file_path.string()});
    }

    return Result<unsigned, SLError>::ok(weight);
}

Result<void, SLError> CgroupV2::set_cpu_weight(unsigned weight) const {
    return write_control_file("cpu.weight", std::to_string(weight));
}

Result<void, SLError> CgroupV2::set_memory_max(size_t bytes) const {
    return write_control_file("memory.max", bytes == 0 ? "max" : std::to_string(bytes));
}

Result<void, SLError> CgroupV2::set_cpuset_cpus(const std::string& cpus) const {
    return write_control_file("cpuset.cpus", cpus);
}

Result<void, SLError> CgroupV2::set_cpuset_mems(const std::string& mems) const {
    return write_control_file("cpuset.mems", mems);
}

Result<void, SLError> CgroupV2::write_control_file(const char* name,
                                                   const std::string& value) const {
    auto file_path = path_ / name;
    std::ofstream f(file_path, std::ios::trunc);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    "cannot write " + file_path.string()});
    }

    f << value << "\n";
    if (f.fail()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal,
                    "write failed for " + file_path.string()});
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight
