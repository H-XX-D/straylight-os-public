// apps/oobe/oobe_state.h
// OOBE flow state machine with crash-recovery persistence
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <string>
#include <string_view>

namespace straylight::oobe {

/// Steps in the OOBE wizard flow.
enum class OobeStep {
    kWelcome,
    kAccount,
    kPackageProfile,
    kNetwork,
    kSummary,
    kDone,
};

/// Converts an OobeStep to its string representation.
const char* step_to_string(OobeStep step);

/// Parses a string to an OobeStep. Returns kWelcome on unknown input.
OobeStep string_to_step(std::string_view s);

/// Persistent state machine for the OOBE wizard.
/// State is written to a JSON file on disk so the wizard can resume
/// after a crash or power loss.
class OobeState {
public:
    /// Load persisted state from disk, or start fresh at kWelcome.
    /// Corrupt or missing file falls back to kWelcome.
    static Result<OobeState, SLError> load(
        std::string_view path = "/var/lib/straylight/oobe_progress.json");

    /// Advance the state machine to the next step.
    void advance(OobeStep next);

    /// Get the current step.
    [[nodiscard]] OobeStep current() const;

    /// Persist current state to disk (atomic: write tmp + fsync + rename).
    void save() const;

    /// Get the state file path.
    [[nodiscard]] const std::string& path() const;

private:
    std::string path_;
    OobeStep step_ = OobeStep::kWelcome;
};

} // namespace straylight::oobe
