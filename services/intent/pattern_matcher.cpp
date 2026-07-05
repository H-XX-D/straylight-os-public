/**
 * StrayLight Pattern Matcher — Implementation.
 *
 * The PatternMatcher is header-only. This translation unit provides an
 * explicit compilation unit for linker purposes and any future non-inline
 * implementations.
 */

#include "pattern_matcher.h"

// Explicit template instantiations and static assertions
namespace straylight::intent {

static_assert(sizeof(PatternMatcher) > 0, "PatternMatcher must be a complete type");

} // namespace straylight::intent
