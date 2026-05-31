#include "About.h"

#include <string>

// Baked in by CMake (defaults to the project() version; CI/release pass the
// release tag). Falls back to "dev" for ad-hoc builds without the definition.
#ifndef SUNDIAL_VERSION
#define SUNDIAL_VERSION "dev"
#endif

namespace sundial {

namespace {
constexpr char kRepoUrl[] = "https://github.com/brendan-duncan/sundial";
}  // namespace

void ShowAbout(HWND owner) {
    const std::string msg =
        "Sundial v" SUNDIAL_VERSION "\r\n"
        "HDR screen capture utility for Windows\r\n\r\n" +
        std::string(kRepoUrl);
    MessageBoxA(owner, msg.c_str(), "About Sundial", MB_ICONINFORMATION | MB_OK);
}

}  // namespace sundial
