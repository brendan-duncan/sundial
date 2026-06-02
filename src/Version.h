#pragma once

#include <string>

namespace sundial {

// This binary's version string, e.g. "1.1.1" - the version baked in by CMake
// from project(VERSION ...) (or the release tag CI passes via -DSUNDIAL_VERSION),
// falling back to "dev" for ad-hoc builds. Computed once on first use.
const std::string& AppVersion();    // UTF-8, e.g. "1.1.1"
const std::wstring& AppVersionW();   // wide,  e.g. L"1.1.1"

}  // namespace sundial
