/**
 * StrayLight Intent Executor — Implementation.
 *
 * The Executor is header-only. This translation unit provides
 * an explicit compilation point.
 */

#include "executor.h"

namespace straylight::intent {

static_assert(sizeof(Executor) > 0, "Executor must be a complete type");

} // namespace straylight::intent
