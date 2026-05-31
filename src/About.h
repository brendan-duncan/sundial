#pragma once
#include <Windows.h>

namespace sundial {

// Shows a small "About Sundial" dialog with the current version and an offer to
// open the project page on GitHub. `owner` may be null.
void ShowAbout(HWND owner);

}  // namespace sundial
