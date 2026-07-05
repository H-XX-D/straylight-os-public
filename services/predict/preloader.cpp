/**
 * StrayLight Preloader — Implementation.
 *
 * The Preloader is header-only. This translation unit provides
 * an explicit compilation point.
 */

#include "preloader.h"

namespace straylight::predict {

static_assert(sizeof(Preloader) > 0, "Preloader must be a complete type");

} // namespace straylight::predict
