/**
 * StrayLight Usage Collector — Implementation.
 *
 * The UsageCollector is header-only. This translation unit provides
 * an explicit compilation point.
 */

#include "usage_collector.h"

namespace straylight::predict {

static_assert(sizeof(UsageCollector) > 0, "UsageCollector must be a complete type");

} // namespace straylight::predict
