/**
 * StrayLight Rewind — Checkpoint Store implementation
 *
 * The bulk of CheckpointStore is header-only (template-heavy Result usage).
 * This TU provides the static members and any out-of-line helpers that
 * benefit from a single compilation unit.
 */

#include "checkpoint_store.h"

// Nothing additional needed — the class is fully defined in the header.
// This TU exists so that CMake has a .cpp to compile for the library target,
// and to serve as the anchor for potential future out-of-line methods.
