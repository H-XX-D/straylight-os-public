#pragma once

// Symbol visibility macros for shared libraries.
// All symbols are hidden by default (CMAKE_CXX_VISIBILITY_PRESET=hidden).
// Use STRAYLIGHT_EXPORT on public API functions/classes.

#if defined(_WIN32)
    #define STRAYLIGHT_EXPORT __declspec(dllexport)
    #define STRAYLIGHT_IMPORT __declspec(dllimport)
#else
    #define STRAYLIGHT_EXPORT __attribute__((visibility("default")))
    #define STRAYLIGHT_IMPORT __attribute__((visibility("default")))
#endif

// Each library defines its own API macro. Example:
// #ifdef straylight_common_EXPORTS
// #define STRAYLIGHT_COMMON_API STRAYLIGHT_EXPORT
// #else
// #define STRAYLIGHT_COMMON_API STRAYLIGHT_IMPORT
// #endif
