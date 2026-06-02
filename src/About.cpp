#include "About.h"

#include <string>

#include "Version.h"

namespace sundial {

namespace {
constexpr char kRepoUrl[] = "https://github.com/brendan-duncan/sundial";
}  // namespace

void ShowAbout(HWND owner) {
    const std::string msg =
        "Sundial v" + AppVersion() + "\r\n" +
        "HDR screen capture utility for Windows\r\n\r\n" +
        std::string(kRepoUrl);
    MessageBoxA(owner, msg.c_str(), "About Sundial", MB_ICONINFORMATION | MB_OK);
}

}  // namespace sundial
