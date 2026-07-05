/**
 * StrayLight Action Registry — Implementation.
 *
 * The ActionRegistry is header-only (all logic in the header) since it uses
 * the Meyer's singleton pattern. This translation unit exists solely to ensure
 * the singleton is instantiated in this compilation unit and to provide an
 * explicit point for linker symbol resolution.
 */

#include "action_registry.h"

// Force singleton instantiation in this TU.
namespace straylight::intent {
    static ActionRegistry& _force_registry_init = ActionRegistry::instance();
} // namespace straylight::intent
