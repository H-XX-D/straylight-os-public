#pragma once

#define STRAYLIGHT_VERSION_MAJOR 1
#define STRAYLIGHT_VERSION_MINOR 0
#define STRAYLIGHT_VERSION_PATCH 0
#define STRAYLIGHT_VERSION "1.0.0"

namespace straylight {

struct Version {
    static constexpr int major = STRAYLIGHT_VERSION_MAJOR;
    static constexpr int minor = STRAYLIGHT_VERSION_MINOR;
    static constexpr int patch = STRAYLIGHT_VERSION_PATCH;
    static constexpr const char* string = STRAYLIGHT_VERSION;
};

} // namespace straylight
