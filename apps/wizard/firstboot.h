// apps/wizard/firstboot.h
// Boot state file utilities for the wizard
#pragma once

#include <string>

namespace straylight::wizard {

/// Check if the current boot state indicates the wizard should run.
/// Reads /var/lib/straylight/state and returns true if it contains "wizard".
bool is_firstboot(const std::string& state_path = "/var/lib/straylight/state");

/// Mark the wizard as complete by writing "complete" to the state file.
/// Uses atomic write (temp + rename).
void mark_complete(const std::string& state_path = "/var/lib/straylight/state");

/// Read the boot state file contents.
std::string read_boot_state(const std::string& state_path = "/var/lib/straylight/state");

} // namespace straylight::wizard
