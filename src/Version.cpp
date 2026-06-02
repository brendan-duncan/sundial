#include "Version.h"

// Baked in by CMake from project(VERSION ...) (or the release tag CI passes via
// -DSUNDIAL_VERSION). Falls back to "dev" for ad-hoc builds without it.
#ifndef SUNDIAL_VERSION
#define SUNDIAL_VERSION "dev"
#endif

namespace sundial {

const std::string& AppVersion() {
    static const std::string version = SUNDIAL_VERSION;
    return version;
}

const std::wstring& AppVersionW() {
    // The version string is ASCII (digits, dots, optional pre-release tag), so
    // a straight char->wchar widen is exact.
    static const std::wstring version(AppVersion().begin(), AppVersion().end());
    return version;
}

}  // namespace sundial
